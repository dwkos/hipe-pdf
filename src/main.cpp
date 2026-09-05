#include <hipe.h>
#include "pdf_document.hpp"

#include <algorithm>
#include <chrono>
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
#define REQ_SLIDESHOW_ENTER 7
#define REQ_SLIDESHOW_LEAVE 8
#define REQ_SLIDESHOW_ADVANCE 9
#define REQ_SLIDESHOW_MENU 10
#define REQ_SLIDESHOW_DIALOG 11
#define REQ_KEYDOWN 12
#define REQ_WHEEL 13
#define REQ_THUMB_BASE 1000

/* DOM KeyboardEvent.keyCode values (legacy, but what this WebKit fork's
 * keydown detail string actually carries -- see requestEvent() in
 * hipecore's qwebelement.cpp). */
#define KEY_PAGEUP 33
#define KEY_PAGEDOWN 34
#define KEY_END 35
#define KEY_HOME 36
#define KEY_ARROWUP 38
#define KEY_ARROWDOWN 40

static const float ARROW_SCROLL_STEP_PX = 60.0f;
static const float THUMB_WIDTH_PX = 110.0f;
static const float ZOOM_MIN = 0.25f;
static const float ZOOM_MAX = 4.0f;
static const float ZOOM_STEP = 1.25f;
static const float VIEWPORT_MARGIN_PX = 20.0f; /* rough allowance for scrollbars/padding */
static const float SHADOW_MARGIN_PX = 14.0f; /* room for img_page's box-shadow blur below the page */

enum class FitMode { NONE, WIDTH, PAGE };

static hipe_session session;
static std::unique_ptr<PdfDocument> doc;
static int current_page = 0;
static int page_count = 0;
static hipe_loc img_page;
static hipe_loc page_wrapper;
static hipe_loc text_layer;
static hipe_loc viewport;
static hipe_loc sidebar;
static hipe_loc navbar;
static hipe_loc main_area;
static hipe_loc slideshow_leave_btn;
static hipe_loc page_label;
static hipe_loc zoom_label;
static float base_render_width = 800.0f;
static float zoom_level = 1.0f;
static float render_width = 800.0f;
static FitMode fit_mode = FitMode::NONE;
static bool slideshow_active = false;
static FitMode saved_fit_mode = FitMode::NONE;
static float saved_zoom_level = 1.0f;
static float wheel_prev_scroll_top = -1.0f;
static bool wheel_stuck = false;
static std::chrono::steady_clock::time_point wheel_stuck_since;
static std::chrono::steady_clock::time_point wheel_cooldown_until;
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

static void apply_fit_mode(float aspect_h_over_w, float viewport_w, float viewport_h) {
	/* Recomputes render_width for the active fit mode against the current viewport
	 * size and (for FitMode::PAGE) the target page's own aspect ratio, since a mixed
	 * portrait/landscape document needs a different width per page to fit fully. */
	if (fit_mode == FitMode::NONE) return;
	if (viewport_w < 50.0f) return; /* not laid out yet; keep the previous width */

	if (fit_mode == FitMode::WIDTH) {
		render_width = viewport_w - VIEWPORT_MARGIN_PX;
	} else if (fit_mode == FitMode::PAGE) {
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

static void update_slideshow_background(int page_number) {
	uint8_t r = 255, g = 255, b = 255;
	doc->pageBackgroundColor(page_number, &r, &g, &b);

	char color_buf[24];
	snprintf(color_buf, sizeof(color_buf), "rgb(%d,%d,%d)", r, g, b);
	/* Standard luminance-based contrast pick, so periscope's own body-colormatched
	 * chrome (and our own leave button) stay legible against light or dark pages. */
	double luminance = 0.299 * r + 0.587 * g + 0.114 * b;
	const char* fg = (luminance > 128.0) ? "black" : "white";

	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background", color_buf);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "background-color", color_buf);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "color", fg);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "color", fg);
}

static void update_text_layer(int page_number, float scale) {
	hipe_send(session, HIPE_OP_CLEAR, 0, text_layer, 0);

	std::vector<PdfDocument::TextSpan> spans;
	try {
		spans = doc->pageTextSpans(page_number);
	} catch (const std::exception& e) {
		fprintf(stderr, "update_text_layer: %s\n", e.what());
		return;
	}

	char buf[16];
	for (size_t i = 0; i < spans.size(); i++) {
		const auto& span = spans[i];
		hipe_send(session, HIPE_OP_APPEND_TAG, 0, text_layer, 1, "span");
		hipe_loc loc = hipe_newest_location();
		hipe_send(session, HIPE_OP_SET_TEXT, 0, loc, 1, span.text.c_str());

		/* Common properties (position:absolute, color:transparent, etc.) come from the
		 * "#textLayer span" rule added once at startup; only per-span geometry needs
		 * setting here. font-size is approximated from line height -- Hipe has no
		 * client-side text-measurement API to CSS-scale each span to its exact glyph
		 * run the way PDF.js does, so this is "close enough to select", not
		 * pixel-perfect. */

		/* Stretch the box (not the font-size/line-height, which stay true to the actual
		 * line) down to the next line's top, so there's no gap between lines for the
		 * mouse to land in with no span underneath. Dragging a selection through such a
		 * gap has no text position for WebKit's hit-testing to resolve to there, and it
		 * can jump to a distant/wrong spot instead of the nearest line. The cap is
		 * generous (4x the line's own height) specifically so this also bridges a normal
		 * paragraph-break gap (blank line), not just same-paragraph line spacing, while
		 * still guarding against bridging into unrelated content across a real
		 * column/section break further down (or, via a same-page column break, back up). */
		float box_height = span.height;
		if (i + 1 < spans.size()) {
			float gap_to_next = spans[i + 1].y - (span.y + span.height);
			if (gap_to_next > 0 && gap_to_next < span.height * 4.0f)
				box_height = spans[i + 1].y - span.y;
		}

		snprintf(buf, sizeof(buf), "%dpx", (int) (span.x * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "left", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (span.y * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "top", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (span.width * scale + 1));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "width", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (box_height * scale + 1));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "height", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (span.height * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "line-height", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (span.height * scale * 0.9f));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "font-size", buf);
	}
}

static void render_and_show(int page_number, bool land_at_bottom = false) {
	if (page_number < 0 || page_number >= page_count) return;
	bool page_changed = (page_number != current_page);
	current_page = page_number;

	float page_w = 0, page_h = 0;
	try {
		doc->pageSize(current_page, &page_w, &page_h);
	} catch (const std::exception& e) {
		fprintf(stderr, "render_and_show: %s\n", e.what());
	}
	float aspect_h_over_w = (page_w > 0) ? (page_h / page_w) : 1.0f;

	float viewport_w = 0, viewport_h = 0;
	get_geometry(viewport, &viewport_w, &viewport_h);

	apply_fit_mode(aspect_h_over_w, viewport_w, viewport_h);

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

	/* page_wrapper is sized to exactly the rendered box -- img_page and text_layer are
	 * absolutely positioned at 100%/100% inside it (see main()), so page_wrapper's own
	 * width/height must be set explicitly: absolutely-positioned children are out of
	 * normal flow and don't contribute to a parent's size the way img_page's own
	 * intrinsic size used to before the text-layer overlay needed this indirection. */
	float render_height = render_width * aspect_h_over_w;
	char size_buf[16];
	snprintf(size_buf, sizeof(size_buf), "%dpx", (int) (render_width + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "width", size_buf);
	snprintf(size_buf, sizeof(size_buf), "%dpx", (int) (render_height + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "height", size_buf);

	/* Center vertically via an explicit margin-top computed from the actual rendered
	 * size, rather than flex/overflow centering: a page bigger than the viewport needs
	 * to stay scrollable from the top, which flex align-items:center can't do reliably
	 * without "safe center" support (unavailable on this WebKit fork). Horizontal
	 * centering still uses plain auto margins, which degrade correctly on their own.
	 * The fixed SHADOW_MARGIN_PX margin-bottom (see main()) is subtracted from the gap
	 * before centering, so img_page's box-shadow always has a bit of room below it --
	 * otherwise it bleeds past #viewport's scrollable content edge and gets clipped
	 * there (visible right where #navbar starts, since it's #viewport's bottom too). */
	float margin_top = 10.0f;
	if (viewport_h > 50.0f && render_height + SHADOW_MARGIN_PX < viewport_h)
		margin_top = (viewport_h - render_height - SHADOW_MARGIN_PX) / 2.0f;
	char margin_top_buf[16];
	snprintf(margin_top_buf, sizeof(margin_top_buf), "%dpx", (int) (margin_top + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-top", margin_top_buf);

	update_text_layer(current_page, render_width / (page_w > 0 ? page_w : render_width));

	if (slideshow_active) update_slideshow_background(current_page);

	update_page_label();
	highlight_thumbnail(current_page);
	wheel_prev_scroll_top = -1.0f; /* fresh content; forget any pinned-edge state from the old page */
	wheel_stuck = false;

	/* The viewport's scroll position is a property of the container, not the image --
	 * it doesn't reset just because we swapped img_page's src, so without this a page
	 * change leaves the new page scrolled to wherever the old one happened to be. A
	 * page-changing navigation lands at the top (or bottom, for backward navigation,
	 * matching a continuous-reading feel); re-rendering the *same* page for a zoom/fit
	 * change must NOT touch scroll position, so this is skipped when page didn't change. */
	if (page_changed)
		hipe_send(session, HIPE_OP_SCROLL_TO, 0, viewport, 3, (char*) nullptr, land_at_bottom ? "100" : "0", "%");
}

static const auto WHEEL_EDGE_DWELL = std::chrono::milliseconds(400);
static const auto WHEEL_NO_SCROLL_COOLDOWN = std::chrono::milliseconds(500);

static void handle_wheel_event() {
	/* This WebKit fork's generic event bridge doesn't expose wheel delta (see
	 * requestEvent() in hipecore's qwebelement.cpp -- non-mouse/keyboard events just
	 * get a "0" detail string), so direction can't be read directly from the event.
	 * Instead: compare scrollTop across consecutive wheel ticks. If it's unchanged and
	 * sitting at an edge, this tick had no scrolling effect, meaning the user is pushing
	 * further past a limit that's already reached. wheel_prev_scroll_top is reset on
	 * every page change so a fresh page always needs its own newly-pinned tick first. */
	hipe_send(session, HIPE_OP_GET_SCROLL_GEOMETRY, 0, viewport, 0);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_GEOMETRY_RETURN);
	float scroll_top = atof(instr.arg[1]);
	float scroll_height = atof(instr.arg[3]);
	hipe_instruction_clear(&instr);

	float viewport_h = 0;
	get_geometry(viewport, nullptr, &viewport_h);

	bool at_top = scroll_top <= 1.0f;
	bool at_bottom = (scroll_top + viewport_h) >= (scroll_height - 1.0f);
	auto now = std::chrono::steady_clock::now();

	if (at_top && at_bottom) {
		/* The whole page already fits with nothing to scroll -- there's no "reached the
		 * edge" moment to pause at, so flip immediately rather than making the user wait
		 * out a dwell timer that has nothing to do with this case. Direction still can't
		 * be inferred (no delta info -- see above), so this always advances forward as a
		 * pragmatic default; a real fix needs hipecore's event bridge to expose
		 * deltaY for wheel events specifically. A short cooldown still applies so one
		 * continuous gesture doesn't cascade through many same-sized short pages. */
		if (now < wheel_cooldown_until) return;
		wheel_cooldown_until = now + WHEEL_NO_SCROLL_COOLDOWN;
		render_and_show(current_page + 1);
		return;
	}

	bool pinned = (wheel_prev_scroll_top >= 0.0f) && (scroll_top == wheel_prev_scroll_top) && (at_top || at_bottom);
	if (!pinned) {
		wheel_stuck = false;
		wheel_prev_scroll_top = scroll_top;
		return;
	}

	if (!wheel_stuck) {
		/* Just arrived pinned at the edge -- start a dwell timer instead of flipping
		 * immediately. A fast scroll-to-bottom gesture fires a burst of wheel ticks
		 * that would otherwise flip the page as its very first bit of feedback, before
		 * the user has had a chance to see they've hit the edge and stop scrolling.
		 * Further ticks that arrive still pinned (whether densely packed in the same
		 * burst or sparser) accumulate against this same timer rather than resetting
		 * it, and a fresh page always starts unpinned, so this also naturally prevents
		 * cascading through multiple pages from one gesture -- no separate post-flip
		 * cooldown is needed here (unlike the no-scroll case above). */
		wheel_stuck = true;
		wheel_stuck_since = now;
		return;
	}
	if (now - wheel_stuck_since < WHEEL_EDGE_DWELL) return; /* still dwelling */

	render_and_show(at_bottom ? current_page + 1 : current_page - 1, /*land_at_bottom=*/ !at_bottom);
}

static void handle_arrow_key(bool down) {
	/* Arrow keys scroll the viewport a step at a time; once already at the edge in
	 * the pressed direction, they turn the page instead. Unlike the wheel, each
	 * keydown (including OS key-repeat while held) is a discrete, intentional
	 * request, so no burst/cooldown guard is needed here. */
	hipe_send(session, HIPE_OP_GET_SCROLL_GEOMETRY, 0, viewport, 0);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_GEOMETRY_RETURN);
	float scroll_top = atof(instr.arg[1]);
	float scroll_height = atof(instr.arg[3]);
	hipe_instruction_clear(&instr);

	float viewport_h = 0;
	get_geometry(viewport, nullptr, &viewport_h);

	bool at_top = scroll_top <= 1.0f;
	bool at_bottom = (scroll_top + viewport_h) >= (scroll_height - 1.0f);

	if (down) {
		if (at_bottom) { render_and_show(current_page + 1); return; }
	} else {
		if (at_top) { render_and_show(current_page - 1, true); return; }
	}

	char step_buf[8];
	snprintf(step_buf, sizeof(step_buf), "%d", down ? (int) ARROW_SCROLL_STEP_PX : -(int) ARROW_SCROLL_STEP_PX);
	hipe_send(session, HIPE_OP_SCROLL_BY, 0, viewport, 2, (char*) nullptr, step_buf);
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

static void enter_slideshow() {
	slideshow_active = true;
	saved_fit_mode = fit_mode;
	saved_zoom_level = zoom_level;

	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "display", "block");
	/* Dropped so the page blends into the color-matched surround instead of standing
	 * out inside a boxed frame -- update_slideshow_background() takes over from here. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "none");
	/* Requesting "contextmenu" always forces preventDefault on the server side (not
	 * something our request controls), suppressing the native menu -- including its
	 * Copy item for a text selection. So this is only requested while actually in
	 * slideshow, not unconditionally at startup, so text stays copyable otherwise. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_MENU, main_area, 1, "contextmenu");

	set_fit_mode(FitMode::PAGE); /* triggers render_and_show, which calls update_slideshow_background */
}

static void leave_slideshow() {
	slideshow_active = false;

	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", "block");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "0 0 12px rgba(0,0,0,0.5)");
	/* Clear back to Hipe's theme default rather than a hardcoded color (empty value
	 * removes the inline override -- see HIPE_OP_SET_STYLE notes in CLAUDE.md). */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background", "");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "background-color", "");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "color", "");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, main_area, 1, "contextmenu");

	if (saved_fit_mode == FitMode::NONE) set_zoom(saved_zoom_level);
	else set_fit_mode(saved_fit_mode);
}

static void show_slideshow_dialog() {
	/* arg[3] symbols line up 1:1 with the arg[2] choices, plus one trailing symbol
	 * for the dialog itself: prev=\xe2\x97\x80, next=\xe2\x96\xb6, start=\xe2\x8f\xae,
	 * end=\xe2\x8f\xad, leave=\xe2\x9c\x95, dialog icon=\xe2\x96\xb6 again. */
	hipe_send(session, HIPE_OP_DIALOG, REQ_SLIDESHOW_DIALOG, 0, 4,
		"Slideshow", "Choose an action:",
		"Previous page\nNext page\nGo to start\nGo to end\nLeave slideshow",
		"\xe2\x97\x80\n\xe2\x96\xb6\n\xe2\x8f\xae\n\xe2\x8f\xad\n\xe2\x9c\x95\n\xe2\x96\xb6");
}

static void handle_slideshow_dialog_return(const hipe_instruction& reply) {
	int choice = reply.arg[1] ? atoi(reply.arg[1]) : 0;
	switch (choice) {
		case 1: render_and_show(current_page - 1, true); break;
		case 2: render_and_show(current_page + 1); break;
		case 3: render_and_show(0); break;
		case 4: render_and_show(page_count - 1); break;
		case 5: leave_slideshow(); break;
		default: break; /* cancelled, or a framing manager without dialog support */
	}
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

	/* No background-color/color here deliberately -- leave body on whatever Hipe's own
	 * theme/CSS (HIPE_THEME, --css) supplies, so the app matches the system theme when
	 * not in slideshow. Slideshow temporarily overrides these (see enter_slideshow). */
	/* user-select:none here (inherited by everything) plus the override back to text
	 * below is what keeps a stray drag-select or "Select all" confined to the actual
	 * page text instead of grabbing the whole GUI (buttons, labels, thumbnails, etc). */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "body",
		"margin:0; font-family:sans-serif; -webkit-user-select:none; user-select:none;");
	/* Shared text-overlay span properties; per-span geometry is set individually in
	 * update_text_layer(). "style" isn't in the server's SET_ATTRIBUTE whitelist, so this
	 * (rather than one combined inline style per span) is how the fixed parts are set. */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "#textLayer span",
		"position:absolute; color:transparent; white-space:nowrap; overflow:hidden; cursor:text; "
		"-webkit-user-select:text; user-select:text;");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, 0, 2, "div", "root");
	hipe_loc root = get_by_id("root");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "flex-direction", "row");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "height", "100vh");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "width", "100vw");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "sidebar");
	sidebar = get_by_id("sidebar");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "width", "140px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "overflow-y", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "background", "#f0f0f0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "border-right", "1px solid #ccc");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "flex-shrink", "0");
	build_thumbnail_sidebar(sidebar);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "main");
	main_area = get_by_id("main");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex-direction", "column");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "overflow", "hidden");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "position", "relative");
	/* No background here either -- left transparent so body's theme-supplied color
	 * shows through behind the page when zoomed out; slideshow overrides it directly. */

	/* #viewport scrolls independently of #navbar below it, so a zoomed-in page can be
	 * panned without the nav controls scrolling out of view. Centering the page image
	 * via auto margins (rather than flex align/justify-center) means the CSS degrades
	 * correctly when the image is bigger than the viewport: auto margins collapse to 0
	 * instead of clipping the overflow unreachably on both sides. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "viewport");
	viewport = get_by_id("viewport");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "overflow", "auto");

	/* #pageWrapper is sized to exactly the rendered image's box (see render_and_show) and
	 * carries the centering margins that used to live on img_page directly; img#page and
	 * #textLayer both sit inside it at 100%/100%, absolutely positioned, so the invisible
	 * text spans line up with the raster underneath regardless of centering/zoom. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, viewport, 2, "div", "pageWrapper");
	page_wrapper = get_by_id("pageWrapper");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "position", "relative");
	/* margin-top is recomputed per render for vertical centering (accounting for the
	 * fixed margin-bottom below -- see render_and_show); left/right stay auto for
	 * horizontal centering. margin-bottom reserves room for img_page's box-shadow blur,
	 * which would otherwise bleed past #viewport's scrollable content edge and get
	 * clipped there once scrolled to the bottom (or once vertically centered with no
	 * gap to spare). */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-left", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-right", "auto");
	char shadow_margin_buf[16];
	snprintf(shadow_margin_buf, sizeof(shadow_margin_buf), "%dpx", (int) SHADOW_MARGIN_PX);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-bottom", shadow_margin_buf);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "img", "page");
	img_page = get_by_id("page");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "background", "white");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "0 0 12px rgba(0,0,0,0.5)");

	/* Experimental: an invisible, selectable text overlay in the Acrobat/PDF.js style --
	 * real text nodes positioned atop the raster image so the underlying content can be
	 * selected/copied, without needing DOM-level SVG rendering. Built per-render in
	 * update_text_layer() from PdfDocument::pageTextSpans(). Since Hipe has no client-side
	 * text-measurement API, spans can't be CSS-scaled to match glyph metrics exactly the
	 * way PDF.js does -- font-size is approximated from line height, so alignment is
	 * "good enough to select the right text", not pixel-perfect. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "div", "textLayer");
	text_layer = get_by_id("textLayer");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "overflow", "hidden");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "navbar");
	navbar = get_by_id("navbar");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "gap", "12px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "margin", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "padding", "4px 8px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "border-radius", "6px");
	/* Fixed toolbar colors, independent of the theme color showing through #viewport
	 * behind the page -- otherwise light text can vanish against a light theme. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "background", "rgba(0,0,0,0.65)");
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

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "slideshowBtn");
	hipe_loc slideshow_btn = get_by_id("slideshowBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, slideshow_btn, 1, "Slideshow \xe2\x96\xb6");

	/* Overlay button, only shown once slideshow mode hides the sidebar/navbar -- a
	 * guaranteed way to exit that doesn't depend on right-click dialog support, which
	 * varies by framing manager (see HIPE_OP_DIALOG notes in CLAUDE.md). */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "button", "slideshowLeaveBtn");
	slideshow_leave_btn = get_by_id("slideshowLeaveBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, slideshow_leave_btn, 1, "\xe2\x9c\x95 Leave Slideshow");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "top", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "right", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "opacity", "0.6");

	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_PREV, prev_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_NEXT, next_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_OUT, zoom_out_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_IN, zoom_in_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_WIDTH, fit_width_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_PAGE, fit_page_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_ENTER, slideshow_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_LEAVE, slideshow_leave_btn, 1, "click");
	/* Registered once for the whole main area; guarded by slideshow_active in the
	 * dispatch loop below rather than requested/cancelled on entering/leaving, since
	 * only one request per (element, event type) pair can be active at a time anyway.
	 * "contextmenu" is the exception -- see enter_slideshow/leave_slideshow. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_ADVANCE, main_area, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_KEYDOWN, 0, 1, "keydown"); /* location 0 = whole-frame keydown */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_WHEEL, viewport, 1, "wheel");

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

		if (event.opcode == HIPE_OP_DIALOG_RETURN) {
			if (event.requestor == REQ_SLIDESHOW_DIALOG) handle_slideshow_dialog_return(event);
			continue;
		}

		if (event.opcode != HIPE_OP_EVENT) continue;
		/* requestor is only meaningful on HIPE_OP_EVENT replies to our own
		 * EVENT_REQUESTs -- other instruction types can carry unrelated
		 * requestor values that happen to collide with our REQ_* codes. */
		if (event.requestor == REQ_PREV) render_and_show(current_page - 1, true);
		else if (event.requestor == REQ_NEXT) render_and_show(current_page + 1);
		else if (event.requestor == REQ_ZOOM_OUT) set_zoom(zoom_level / ZOOM_STEP);
		else if (event.requestor == REQ_ZOOM_IN) set_zoom(zoom_level * ZOOM_STEP);
		else if (event.requestor == REQ_FIT_WIDTH) set_fit_mode(FitMode::WIDTH);
		else if (event.requestor == REQ_FIT_PAGE) set_fit_mode(FitMode::PAGE);
		else if (event.requestor == REQ_SLIDESHOW_ENTER) enter_slideshow();
		else if (event.requestor == REQ_SLIDESHOW_LEAVE) leave_slideshow();
		else if (event.requestor == REQ_SLIDESHOW_ADVANCE && slideshow_active) render_and_show(current_page + 1);
		else if (event.requestor == REQ_SLIDESHOW_MENU && slideshow_active) show_slideshow_dialog();
		else if (event.requestor == REQ_KEYDOWN) {
			switch (event.arg[1] ? atoi(event.arg[1]) : 0) {
				case KEY_PAGEUP: render_and_show(current_page - 1, true); break;
				case KEY_PAGEDOWN: render_and_show(current_page + 1); break;
				case KEY_HOME: render_and_show(0); break;
				case KEY_END: render_and_show(page_count - 1); break;
				case KEY_ARROWUP: handle_arrow_key(false); break;
				case KEY_ARROWDOWN: handle_arrow_key(true); break;
			}
		}
		else if (event.requestor == REQ_WHEEL) handle_wheel_event();
		else if (event.requestor >= REQ_THUMB_BASE) render_and_show((int) (event.requestor - REQ_THUMB_BASE));
	} while (event.opcode != HIPE_OP_FRAME_CLOSE);

	hipe_close_session(session);
	return 0;
}
