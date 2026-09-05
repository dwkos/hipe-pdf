#include <hipe.h>
#include "pdf_document.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define REQ_PREV 1
#define REQ_NEXT 2
#define REQ_ZOOM_OUT 3
#define REQ_ZOOM_IN 4
#define REQ_FIT_WIDTH 5
#define REQ_FIT_PAGE 6
#define REQ_THUMB_BASE 1000

static const float THUMB_WIDTH_PX = 110.0f;
static const float ZOOM_MIN = 0.25f;
static const float ZOOM_MAX = 4.0f;
static const float ZOOM_STEP = 1.25f;
static const float VIEWPORT_MARGIN_PX = 20.0f; /* rough allowance for scrollbars/padding */

enum class FitMode { NONE, WIDTH, PAGE };

static hipe_session session;
static std::unique_ptr<PdfDocument> doc;
static int current_page = 0;
static int page_count = 0;
static hipe_loc img_page;
static hipe_loc viewport;
static hipe_loc page_label;
static hipe_loc zoom_label;
static float base_render_width = 800.0f;
static float zoom_level = 1.0f;
static float render_width = 800.0f;
static FitMode fit_mode = FitMode::NONE;
static std::vector<hipe_loc> thumb_locs;

static hipe_loc get_by_id(const char* id) {
	hipe_send(session, HIPE_OP_GET_BY_ID, 0, 0, 1, id);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_LOCATION_RETURN);
	hipe_loc loc = instr.location;
	hipe_instruction_clear(&instr);
	return loc;
}

static void get_geometry(hipe_loc target, float* out_w, float* out_h) {
	hipe_send(session, HIPE_OP_GET_GEOMETRY, 0, target, 0);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_GEOMETRY_RETURN);
	if (out_w) *out_w = atof(instr.arg[2]);
	if (out_h) *out_h = atof(instr.arg[3]);
	hipe_instruction_clear(&instr);
}

static void update_page_label() {
	char buf[64];
	snprintf(buf, sizeof(buf), "Page %d / %d", current_page + 1, page_count);
	hipe_send(session, HIPE_OP_SET_TEXT, 0, page_label, 1, buf);
}

static void update_zoom_label() {
	char buf[16];
	if (fit_mode == FitMode::WIDTH) snprintf(buf, sizeof(buf), "Fit W");
	else if (fit_mode == FitMode::PAGE) snprintf(buf, sizeof(buf), "Fit Pg");
	else snprintf(buf, sizeof(buf), "%d%%", (int) (zoom_level * 100.0f + 0.5f));
	hipe_send(session, HIPE_OP_SET_TEXT, 0, zoom_label, 1, buf);
}

static void apply_fit_mode(int page_number) {
	/* Recomputes render_width for the active fit mode against the current viewport
	 * size and (for FitMode::PAGE) the target page's own aspect ratio, since a mixed
	 * portrait/landscape document needs a different width per page to fit fully. */
	if (fit_mode == FitMode::NONE) return;

	float viewport_w = 0, viewport_h = 0;
	get_geometry(viewport, &viewport_w, &viewport_h);
	if (viewport_w < 50.0f) return; /* not laid out yet; keep the previous width */

	if (fit_mode == FitMode::WIDTH) {
		render_width = viewport_w - VIEWPORT_MARGIN_PX;
	} else if (fit_mode == FitMode::PAGE) {
		float page_w = 0, page_h = 0;
		try {
			doc->pageSize(page_number, &page_w, &page_h);
		} catch (const std::exception& e) {
			fprintf(stderr, "apply_fit_mode: %s\n", e.what());
			return;
		}
		float aspect_h_over_w = (page_w > 0) ? (page_h / page_w) : 1.0f;
		float width_for_height_fit = (viewport_h - VIEWPORT_MARGIN_PX) / aspect_h_over_w;
		render_width = std::min(viewport_w - VIEWPORT_MARGIN_PX, width_for_height_fit);
	}

	render_width = std::max(render_width, 50.0f);
	zoom_level = render_width / base_render_width;
}

static void highlight_thumbnail(int page_number) {
	static int previous = -1;
	if (previous >= 0 && previous < (int) thumb_locs.size())
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb_locs[previous], 2, "border", "2px solid transparent");
	if (page_number >= 0 && page_number < (int) thumb_locs.size())
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb_locs[page_number], 2, "border", "2px solid #3388ff");
	previous = page_number;
}

static void render_and_show(int page_number) {
	if (page_number < 0 || page_number >= page_count) return;
	current_page = page_number;
	apply_fit_mode(current_page);

	std::vector<uint8_t> png;
	try {
		png = doc->renderPagePng(current_page, render_width);
	} catch (const std::exception& e) {
		fprintf(stderr, "Failed to render page %d: %s\n", current_page, e.what());
		return;
	}

	hipe_instruction instr;
	hipe_instruction_init(&instr);
	instr.opcode = HIPE_OP_SET_SRC;
	instr.location = img_page;
	instr.arg[0] = reinterpret_cast<char*>(png.data());
	instr.arg_length[0] = png.size();
	instr.arg[1] = (char*) "image/png";
	instr.arg_length[1] = strlen(instr.arg[1]);
	hipe_send_instruction(session, instr);

	char width_buf[16];
	snprintf(width_buf, sizeof(width_buf), "%dpx", (int) (render_width + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "width", width_buf);

	update_page_label();
	highlight_thumbnail(current_page);
}

static void set_zoom(float new_zoom) {
	fit_mode = FitMode::NONE; /* manual zoom overrides any active fit mode */
	if (new_zoom < ZOOM_MIN) new_zoom = ZOOM_MIN;
	if (new_zoom > ZOOM_MAX) new_zoom = ZOOM_MAX;
	zoom_level = new_zoom;
	render_width = base_render_width * zoom_level;
	render_and_show(current_page);
	update_zoom_label();
}

static void set_fit_mode(FitMode mode) {
	fit_mode = mode;
	render_and_show(current_page);
	update_zoom_label();
}

static void build_thumbnail_sidebar(hipe_loc sidebar) {
	/* Eagerly rendered up front; fine for typical documents. A lazy,
	 * scroll-driven variant (matching the continuous-scroll stretch goal)
	 * would be needed for very large page counts. */
	thumb_locs.reserve(page_count);
	for (int i = 0; i < page_count; i++) {
		std::vector<uint8_t> thumb_png;
		try {
			thumb_png = doc->renderPagePng(i, THUMB_WIDTH_PX);
		} catch (const std::exception& e) {
			fprintf(stderr, "Failed to render thumbnail %d: %s\n", i, e.what());
			thumb_locs.push_back(0);
			continue;
		}

		hipe_send(session, HIPE_OP_APPEND_TAG, 0, sidebar, 1, "img");
		hipe_loc thumb = hipe_newest_location();
		thumb_locs.push_back(thumb);

		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "display", "block");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "width", "110px");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "margin", "6px auto");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "border", "2px solid transparent");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "cursor", "pointer");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "box-shadow", "0 0 4px rgba(0,0,0,0.3)");

		hipe_instruction instr;
		hipe_instruction_init(&instr);
		instr.opcode = HIPE_OP_SET_SRC;
		instr.location = thumb;
		instr.arg[0] = reinterpret_cast<char*>(thumb_png.data());
		instr.arg_length[0] = thumb_png.size();
		instr.arg[1] = (char*) "image/png";
		instr.arg_length[1] = strlen(instr.arg[1]);
		hipe_send_instruction(session, instr);

		hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_THUMB_BASE + i, thumb, 1, "click");
	}
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file.pdf>\n", argv[0]);
		return 1;
	}

	try {
		doc = std::make_unique<PdfDocument>(argv[1]);
	} catch (const std::exception& e) {
		fprintf(stderr, "%s\n", e.what());
		return 2;
	}

	page_count = doc->pageCount();
	if (page_count <= 0) {
		fprintf(stderr, "No pages found in '%s'\n", argv[1]);
		return 2;
	}

	session = hipe_open_session(0, 0, 0, argv[0]);
	if (!session) return 3;

	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "body", "margin:0; font-family:sans-serif;");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, 0, 2, "div", "root");
	hipe_loc root = get_by_id("root");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "flex-direction", "row");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "height", "100vh");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "width", "100vw");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "sidebar");
	hipe_loc sidebar = get_by_id("sidebar");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "width", "140px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "overflow-y", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "background", "#f0f0f0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "border-right", "1px solid #ccc");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "flex-shrink", "0");
	build_thumbnail_sidebar(sidebar);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "main");
	hipe_loc main_area = get_by_id("main");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex-direction", "column");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "overflow", "hidden");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background", "#333333");

	/* #viewport scrolls independently of #navbar below it, so a zoomed-in page can be
	 * panned without the nav controls scrolling out of view. Centering the page image
	 * via auto margins (rather than flex align/justify-center) means the CSS degrades
	 * correctly when the image is bigger than the viewport: auto margins collapse to 0
	 * instead of clipping the overflow unreachably on both sides. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "viewport");
	viewport = get_by_id("viewport");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "overflow", "auto");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, viewport, 2, "img", "page");
	img_page = get_by_id("page");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "display", "block");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "height", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "margin", "10px auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "background", "white");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "0 0 12px rgba(0,0,0,0.5)");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "navbar");
	hipe_loc navbar = get_by_id("navbar");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "gap", "12px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "margin-top", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "color", "white");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "prevBtn");
	hipe_loc prev_btn = get_by_id("prevBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, prev_btn, 1, "\xe2\x80\xb9 Prev");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "span", "pageLabel");
	page_label = get_by_id("pageLabel");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "nextBtn");
	hipe_loc next_btn = get_by_id("nextBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, next_btn, 1, "Next \xe2\x80\xba");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "zoomOutBtn");
	hipe_loc zoom_out_btn = get_by_id("zoomOutBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, zoom_out_btn, 1, "\xe2\x88\x92");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "span", "zoomLabel");
	zoom_label = get_by_id("zoomLabel");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "zoomInBtn");
	hipe_loc zoom_in_btn = get_by_id("zoomInBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, zoom_in_btn, 1, "+");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "fitWidthBtn");
	hipe_loc fit_width_btn = get_by_id("fitWidthBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, fit_width_btn, 1, "Fit W");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "fitPageBtn");
	hipe_loc fit_page_btn = get_by_id("fitPageBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, fit_page_btn, 1, "Fit Page");

	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_PREV, prev_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_NEXT, next_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_OUT, zoom_out_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_IN, zoom_in_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_WIDTH, fit_width_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_PAGE, fit_page_btn, 1, "click");

	/* Size the initial render to roughly fill the main content area. */
	float main_w = 0, main_h = 0;
	get_geometry(main_area, &main_w, &main_h);
	if (main_w > 100) base_render_width = main_w * 0.9f;
	render_width = base_render_width * zoom_level;

	render_and_show(0);
	update_zoom_label();

	hipe_instruction event;
	hipe_instruction_init(&event);
	do {
		hipe_next_instruction(session, &event, 1);
		if (event.opcode != HIPE_OP_EVENT) continue;
		/* requestor is only meaningful on HIPE_OP_EVENT replies to our own
		 * EVENT_REQUESTs -- other instruction types can carry unrelated
		 * requestor values that happen to collide with our REQ_* codes. */
		if (event.requestor == REQ_PREV) render_and_show(current_page - 1);
		else if (event.requestor == REQ_NEXT) render_and_show(current_page + 1);
		else if (event.requestor == REQ_ZOOM_OUT) set_zoom(zoom_level / ZOOM_STEP);
		else if (event.requestor == REQ_ZOOM_IN) set_zoom(zoom_level * ZOOM_STEP);
		else if (event.requestor == REQ_FIT_WIDTH) set_fit_mode(FitMode::WIDTH);
		else if (event.requestor == REQ_FIT_PAGE) set_fit_mode(FitMode::PAGE);
		else if (event.requestor >= REQ_THUMB_BASE) render_and_show((int) (event.requestor - REQ_THUMB_BASE));
	} while (event.opcode != HIPE_OP_FRAME_CLOSE);

	hipe_close_session(session);
	return 0;
}
