#include <hipe.h>
#include "pdf_document.hpp"
#include "icon_data.hpp"

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
#define REQ_RESIZE 14
#define REQ_ZOOM_RESET 15
#define REQ_SIDEBAR_TOGGLE 16
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
/* Room below the page for img_page's box-shadow (see main()/leave_slideshow) -- its dark
 * component is offset 3px down-right with a 10px blur, reaching 13px past the page's own
 * bottom edge, plus a bit of buffer. The complementary light component is offset up-left
 * instead, so it doesn't add to this. */
static const float SHADOW_MARGIN_PX = 20.0f;
static const float SIDEBAR_WIDTH_PX = 140.0f;

enum class FitMode { NONE, WIDTH, PAGE };

static hipe_session session;
static std::unique_ptr<PdfDocument> doc;
static int current_page = 0;
static int page_count = 0;
static hipe_loc img_page;
static hipe_loc page_wrapper;
static hipe_loc bottom_spacer;
static hipe_loc page_status;
static hipe_loc text_layer;
static hipe_loc viewport;
static hipe_loc sidebar;
static hipe_loc navbar;
static hipe_loc main_area;
static hipe_loc slideshow_leave_btn;
static hipe_loc page_label;
static hipe_loc zoom_label;
static float navbar_clearance_px = 0.0f; /* measured once after navbar is built -- see main() */
static float base_render_width = 800.0f;
static float zoom_level = 1.0f;
static float render_width = 800.0f;
static FitMode fit_mode = FitMode::NONE;
static bool slideshow_active = false;
static bool sidebar_visible = true;
static FitMode saved_fit_mode = FitMode::NONE;
static float saved_zoom_level = 1.0f;
/* Tracks whether img_page currently holds a raster that's valid to preview for
 * current_page -- i.e. whether render_and_show can skip the loading placeholder and
 * just let the existing raster stretch to the new page_wrapper box (its 100%/100%
 * sizing already does this for free) while a properly-sized replacement renders,
 * versus needing to show the placeholder because there's nothing sensible to show yet.
 * Conceptually three states collapse to this one bool: no raster at all yet (startup),
 * a raster for a DIFFERENT page (just navigated, stale and not preview-worthy), and a
 * raster for THIS page (safe to stretch-preview, e.g. a zoom/fit change) -- the first
 * two both mean false. Set false whenever the page actually changes (see
 * render_and_show), true right after a successful render, left untouched on failure. */
static bool current_page_has_raster = false;
static bool wheel_stuck = false;
static std::chrono::steady_clock::time_point wheel_stuck_since;
static std::chrono::steady_clock::time_point wheel_cooldown_until;
static std::chrono::steady_clock::time_point last_render_finished_at;
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

static void get_geometry(hipe_loc target, float* out_w, float* out_h, float* out_x = nullptr, float* out_y = nullptr) {
	hipe_send(session, HIPE_OP_GET_GEOMETRY, 0, target, 0);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_GEOMETRY_RETURN);
	if (out_x) *out_x = atof(instr.arg[0]);
	if (out_y) *out_y = atof(instr.arg[1]);
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

	/* VIEWPORT_MARGIN_PX is a rough allowance for scrollbars/padding in the normal
	 * windowed view -- slideshow has neither (sidebar/navbar are hidden, and the page is
	 * meant to fill the frame edge-to-edge), so reserving it there just leaves an
	 * unwanted gap around the page instead of a true fullscreen fit. */
	float margin = slideshow_active ? 0.0f : VIEWPORT_MARGIN_PX;

	if (fit_mode == FitMode::WIDTH) {
		render_width = viewport_w - margin;
	} else if (fit_mode == FitMode::PAGE) {
		/* The floating #navbar overlays #viewport rather than shrinking it (see
		 * #viewport/#navbar in main()), so viewport_h alone would let a fully-fit page's
		 * top edge land underneath the toolbar. navbar_clearance_px (skipped in
		 * slideshow, where navbar is hidden entirely) keeps the whole page below it. */
		float navbar_room = slideshow_active ? 0.0f : navbar_clearance_px;
		float width_for_height_fit = (viewport_h - margin - navbar_room) / aspect_h_over_w;
		render_width = std::min(viewport_w - margin, width_for_height_fit);
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

static void scroll_thumbnail_into_view(int page_number) {
	if (page_number < 0 || page_number >= (int) thumb_locs.size()) return;
	hipe_loc thumb = thumb_locs[page_number];
	if (!thumb) return;

	float sidebar_h = 0, sidebar_y = 0;
	get_geometry(sidebar, nullptr, &sidebar_h, nullptr, &sidebar_y);
	float thumb_h = 0, thumb_y = 0;
	get_geometry(thumb, nullptr, &thumb_h, nullptr, &thumb_y);

	hipe_send(session, HIPE_OP_GET_SCROLL_GEOMETRY, 0, sidebar, 0);
	hipe_instruction instr;
	hipe_instruction_init(&instr);
	hipe_await_instruction(session, &instr, HIPE_OP_GEOMETRY_RETURN);
	float scroll_top = atof(instr.arg[1]);
	hipe_instruction_clear(&instr);

	/* GET_GEOMETRY reports a scroll-independent position (confirmed empirically: it
	 * doesn't change as the sidebar is scrolled), so thumb_y/sidebar_y already give the
	 * thumbnail's offset within the sidebar's scrollable content directly -- no need to
	 * (and it'd be wrong to) add the current scroll_top on top of that. */
	float thumb_offset = thumb_y - sidebar_y;

	float new_scroll_top;
	if (thumb_offset < scroll_top) new_scroll_top = thumb_offset; /* scrolled above the view */
	else if (thumb_offset + thumb_h > scroll_top + sidebar_h) new_scroll_top = thumb_offset + thumb_h - sidebar_h; /* below */
	else return; /* already fully visible */

	if (new_scroll_top < 0) new_scroll_top = 0;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", (int) new_scroll_top);
	hipe_send(session, HIPE_OP_SCROLL_TO, 0, sidebar, 2, (char*) nullptr, buf);
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

static void show_page_status(const char* message) {
	hipe_send(session, HIPE_OP_SET_TEXT, 0, page_status, 1, message);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "display", "flex");
}

static void hide_page_status() {
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "display", "none");
}

static void render_and_show(int page_number, bool land_at_bottom = false) {
	if (page_number < 0 || page_number >= page_count) return;
	bool page_changed = (page_number != current_page);
	current_page = page_number;
	if (page_changed) current_page_has_raster = false; /* whatever img_page shows now is for a different page */

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

	/* page_wrapper is sized to exactly the rendered box -- img_page/text_layer/page_status
	 * are absolutely positioned at 100%/100% inside it (see main()), so page_wrapper's own
	 * width/height must be set explicitly: absolutely-positioned children are out of
	 * normal flow and don't contribute to a parent's size the way img_page's own
	 * intrinsic size used to before the text-layer overlay needed this indirection. This
	 * only depends on the page's known dimensions/fit mode, not on the render below
	 * actually succeeding, so it's done up front -- the loading placeholder already sits
	 * in the right box instead of a stale one. */
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
	 * top_reserve is a MINIMUM margin-top, not just a centering input: the floating
	 * #navbar docks at #viewport's top edge (see main()), so scrolling to the page's
	 * actual top must never bring it flush with the top edge, or it'd land right under
	 * the toolbar. Any extra room beyond that minimum still centers the page in the
	 * remaining space. bottom_reserve is unrelated -- just img_page's box-shadow blur,
	 * which would otherwise bleed past #viewport's scrollable content edge and get
	 * clipped there -- see #bottomSpacer in main() for why this is a sibling element's
	 * height rather than page_wrapper's own margin-bottom. Slideshow disables the shadow
	 * and hides navbar entirely (see enter_slideshow) and wants a true edge-to-edge fit,
	 * so neither is reserved there. */
	float top_reserve = slideshow_active ? 0.0f : navbar_clearance_px;
	float bottom_reserve = slideshow_active ? 0.0f : SHADOW_MARGIN_PX;
	float margin_top = top_reserve;
	if (viewport_h > 50.0f && render_height + top_reserve + bottom_reserve < viewport_h)
		margin_top = top_reserve + (viewport_h - render_height - top_reserve - bottom_reserve) / 2.0f;
	char margin_top_buf[16];
	snprintf(margin_top_buf, sizeof(margin_top_buf), "%dpx", (int) (margin_top + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-top", margin_top_buf);
	char spacer_h_buf[16];
	snprintf(spacer_h_buf, sizeof(spacer_h_buf), "%dpx", (int) (bottom_reserve + 0.5f));
	hipe_send(session, HIPE_OP_SET_STYLE, 0, bottom_spacer, 2, "height", spacer_h_buf);

	/* A complex page's render can take several seconds (see PdfDocument::renderPagePng's
	 * timeout) -- normally that means showing a placeholder immediately rather than
	 * leaving the previous page's now-stale image on screen with no feedback while this
	 * call blocks. But if img_page already holds a raster for THIS SAME page (a zoom/fit
	 * change re-rendering the page we're already on, not a navigation to a new one), that
	 * raster is still perfectly good to look at -- it's already stretching to fill
	 * page_wrapper's just-updated box via its own 100%/100% sizing above, so it works as
	 * an instant preview of roughly the new size with zero extra code, instead of
	 * replacing it with a blank loading screen. hipe_send() writes straight to the
	 * socket with no client-side buffering, so when the placeholder IS needed, it
	 * reaches the display before the CPU-bound render below starts. */
	if (!current_page_has_raster) {
		/* Drop the old page's text spans now rather than leaving them selectable
		 * underneath the overlay. */
		hipe_send(session, HIPE_OP_CLEAR, 0, text_layer, 0);
		char loading_buf[64];
		snprintf(loading_buf, sizeof(loading_buf), "Loading page %d / %d...", current_page + 1, page_count);
		show_page_status(loading_buf);
		update_page_label();
		highlight_thumbnail(current_page);
	}

	std::vector<uint8_t> png;
	try {
		png = doc->renderPagePng(current_page, render_width);
	} catch (const std::exception& e) {
		fprintf(stderr, "Failed to render page %d: %s\n", current_page, e.what());
		char err_buf[128];
		snprintf(err_buf, sizeof(err_buf),
			"Page %d could not be rendered\n(ludicrously complex, or timed out)", current_page + 1);
		show_page_status(err_buf);

		/* current_page really has moved to the failed page (so a repeated Next/Prev keeps
		 * making forward progress instead of getting stuck) -- keep the label/thumbnail
		 * highlight/scroll state in sync with that rather than silently leaving them
		 * pointing at the old page while the status overlay shows a different one. */
		update_page_label();
		highlight_thumbnail(current_page);
		scroll_thumbnail_into_view(current_page);
		wheel_stuck = false;
		last_render_finished_at = std::chrono::steady_clock::now();
		if (page_changed)
			hipe_send(session, HIPE_OP_SCROLL_TO, 0, viewport, 3, (char*) nullptr, "0", "%");
		return;
	}
	hide_page_status();

	hipe_instruction instr;
	hipe_instruction_init(&instr);
	instr.opcode = HIPE_OP_SET_SRC;
	instr.location = img_page;
	instr.arg[0] = reinterpret_cast<char*>(png.data());
	instr.arg_length[0] = png.size();
	instr.arg[1] = (char*) "image/png";
	instr.arg_length[1] = strlen(instr.arg[1]);
	hipe_send_instruction(session, instr);
	current_page_has_raster = true;

	update_text_layer(current_page, render_width / (page_w > 0 ? page_w : render_width));

	if (slideshow_active) update_slideshow_background(current_page);

	update_page_label();
	highlight_thumbnail(current_page);
	scroll_thumbnail_into_view(current_page);
	wheel_stuck = false; /* fresh content; forget any pinned-edge state from the old page */
	last_render_finished_at = std::chrono::steady_clock::now();

	/* The viewport's scroll position is a property of the container, not the image --
	 * it doesn't reset just because we swapped img_page's src, so without this a page
	 * change leaves the new page scrolled to wherever the old one happened to be. A
	 * page-changing navigation lands at the top (or bottom, for backward navigation,
	 * matching a continuous-reading feel); re-rendering the *same* page for a zoom/fit
	 * change must NOT touch scroll position, so this is skipped when page didn't change. */
	if (page_changed)
		hipe_send(session, HIPE_OP_SCROLL_TO, 0, viewport, 3, (char*) nullptr, land_at_bottom ? "100" : "0", "%");
}

static const auto WHEEL_EDGE_DWELL = std::chrono::milliseconds(250);
static const auto WHEEL_NO_SCROLL_COOLDOWN = std::chrono::milliseconds(500);
static const auto WHEEL_RENDER_SETTLE = std::chrono::milliseconds(300);

/* wheel event details are "deltaX,deltaY,deltaMode" (see requestEvent() in hipecore's
 * qwebelement.cpp) -- only the sign of deltaY matters here, regardless of deltaMode
 * (pixel/line/page), so no unit conversion is needed. */
static float parse_wheel_delta_y(const char* details) {
	if (!details) return 0.0f;
	const char* comma = strchr(details, ',');
	if (!comma) return 0.0f;
	return atof(comma + 1);
}

static void handle_wheel_event(float delta_y) {
	/* hipecore's event bridge now reports real wheel deltas, so direction comes straight
	 * from the event instead of being inferred by comparing consecutive scrollTop values
	 * across ticks. Native scrolling still happens on its own (wheel isn't
	 * preventDefault'd), so this just watches for the viewport already being at the edge
	 * in the direction the wheel is pushing, and turns the page there. */
	if (delta_y == 0.0f) return;

	/* A slow render (see PdfDocument::renderPagePng's timeout) blocks this whole client
	 * for as long as it takes, during which any further wheel ticks the user sends just
	 * queue up unprocessed. Once the render returns and the event loop gets back around
	 * to them, they'd otherwise be evaluated against the page we just landed on -- stale
	 * leftover momentum from the very gesture that caused this page turn, misread as a
	 * fresh one and prone to immediately flipping again (especially if the new page has
	 * no scrollable overflow, where a single tick is enough to turn the page). Ignoring
	 * wheel ticks for a brief window after any render finishes discards that backlog
	 * without needing to peek/unread instructions off the wire. */
	if (std::chrono::steady_clock::now() - last_render_finished_at < WHEEL_RENDER_SETTLE) return;

	bool wants_forward = delta_y > 0;

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
		/* Nothing to scroll at all -- no "reached the edge" moment to pause at, so flip
		 * immediately (in the real direction) rather than making the user wait out a
		 * dwell timer that has nothing to do with this case. A short cooldown still
		 * applies so one continuous gesture doesn't cascade through many short pages. */
		if (now < wheel_cooldown_until) return;
		wheel_cooldown_until = now + WHEEL_NO_SCROLL_COOLDOWN;
		render_and_show(wants_forward ? current_page + 1 : current_page - 1, /*land_at_bottom=*/ !wants_forward);
		return;
	}

	bool pinned = (wants_forward && at_bottom) || (!wants_forward && at_top);
	if (!pinned) {
		wheel_stuck = false;
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

	render_and_show(wants_forward ? current_page + 1 : current_page - 1, /*land_at_bottom=*/ !wants_forward);
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

static void toggle_sidebar() {
	sidebar_visible = !sidebar_visible;
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", sidebar_visible ? "block" : "none");
	/* navbar is position:fixed with "left" hardcoded to clear the sidebar (see main()) --
	 * that offset needs to collapse back to 0 when the sidebar's hidden, or the toolbar
	 * would sit indented over empty space for no reason. */
	if (sidebar_visible) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%dpx", (int) SIDEBAR_WIDTH_PX);
		hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "left", buf);
	} else {
		hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "left", "0");
	}
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
	/* Same reasoning for "click": requesting it only from here (not unconditionally at
	 * startup) means the click that just entered slideshow -- which bubbles from the
	 * Slideshow button up through navbar to main_area -- has already finished bubbling
	 * before this listener exists, so it doesn't also immediately advance the very page
	 * we just switched to. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_ADVANCE, main_area, 1, "click");

	set_fit_mode(FitMode::PAGE); /* triggers render_and_show, which calls update_slideshow_background */
}

static void leave_slideshow() {
	slideshow_active = false;

	/* Respects sidebar_visible rather than forcing it back on -- a user who'd toggled the
	 * sidebar off before entering slideshow shouldn't have that choice silently undone by
	 * leaving it again. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", sidebar_visible ? "block" : "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "3px 3px 6px rgba(0,0,0,0.55), -6px -6px 5px rgba(255,255,255,0.2)");
	/* Clear back to Hipe's theme default rather than a hardcoded color (empty value
	 * removes the inline override -- see HIPE_OP_SET_STYLE notes in CLAUDE.md). */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background", "");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "background-color", "");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "color", "");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, main_area, 1, "contextmenu");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, main_area, 1, "click");

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

	{
		hipe_instruction icon_instr;
		hipe_instruction_init(&icon_instr);
		icon_instr.opcode = HIPE_OP_SET_ICON;
		icon_instr.arg[0] = reinterpret_cast<char*>(const_cast<unsigned char*>(kAppIconPng));
		icon_instr.arg_length[0] = kAppIconPngLen;
		hipe_send_instruction(session, icon_instr);
	}

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
	 * (rather than one combined inline style per span) is how the fixed parts are set.
	 * overflow:visible (not hidden) matters here: font-size is only approximated from
	 * line height (no client-side text-measurement API to size it exactly), so the
	 * invisible text can render wider than its box's width, computed from the real PDF
	 * line bbox. With overflow:hidden that excess got clipped away and was unreachable
	 * by the mouse entirely; visible lets it render (still invisibly) past the box at
	 * its natural position, staying selectable there. #textLayer's own overflow:hidden
	 * still bounds everything to the page itself, just not per span. */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "#textLayer span",
		"position:absolute; color:transparent; white-space:nowrap; overflow:visible; cursor:text; "
		"-webkit-user-select:text; user-select:text;");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, 0, 2, "div", "root");
	hipe_loc root = get_by_id("root");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "flex-direction", "row");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "height", "100vh");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "width", "100vw");
	/* background-color: inherit, chained all the way down through main_area/viewport to
	 * sidebar and navbar below (see those), is what lets both pick up Hipe's theme
	 * background/fg color (HIPE_THEME, --css) rather than a hardcoded color of their
	 * own -- background-color isn't naturally inherited like color is, so every link in
	 * the chain has to ask for it explicitly or the lookup stops at "transparent". */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, root, 2, "background-color", "inherit");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "sidebar");
	sidebar = get_by_id("sidebar");
	char sidebar_width_buf[16];
	snprintf(sidebar_width_buf, sizeof(sidebar_width_buf), "%dpx", (int) SIDEBAR_WIDTH_PX);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "width", sidebar_width_buf);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "overflow-y", "auto");
	/* Matches body's theme color (see root above) instead of a hardcoded gray, so it
	 * reads as one piece of chrome with navbar and with periscope's own body-colormatched
	 * frame around it. A drop shadow (rather than the old flat border) is what actually
	 * separates it from the document now -- falling rightward onto #main_area, and
	 * meeting navbar's own downward shadow at the shared top-left corner so the two read
	 * as a single L-shaped panel rather than two separately-decorated strips. Paired dark
	 * + light shadows (rather than just the dark one) borrows periscope's own tooltip/
	 * button technique (~/periscope/hipe.css) for the same reason it needs it there: with
	 * sidebar and #main_area now the same inherited theme color, a shadow of only one
	 * brightness can disappear entirely against whichever theme (dark or light) matches
	 * its own tone, so both are stacked to guarantee contrast either way. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "background-color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "box-shadow", "2px 0 6px rgba(0,0,0,0.35), 2px 0 6px rgba(255,255,255,0.12)");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "position", "relative");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "z-index", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "flex-shrink", "0");
	build_thumbnail_sidebar(sidebar);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "main");
	main_area = get_by_id("main");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex-direction", "column");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "overflow", "hidden");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "position", "relative");
	/* inherit rather than the old bare transparent -- same visual result when nothing
	 * else overrides it (body's color still shows through around the page when zoomed
	 * out), but this is also the link slideshow's own explicit background (see
	 * update_slideshow_background/leave_slideshow) falls back to when cleared. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background-color", "inherit");

	/* #viewport is now the sole child of #main_area (the toolbar floats inside it, see
	 * navbar below) so flex:1 gives it the full frame height. Centering the page image
	 * via auto margins (rather than flex align/justify-center) means the CSS degrades
	 * correctly when the image is bigger than the viewport: auto margins collapse to 0
	 * instead of clipping the overflow unreachably on both sides. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "viewport");
	viewport = get_by_id("viewport");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "overflow", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, viewport, 2, "background-color", "inherit");

	/* #pageWrapper is sized to exactly the rendered image's box (see render_and_show) and
	 * carries the centering margins that used to live on img_page directly; img#page and
	 * #textLayer both sit inside it at 100%/100%, absolutely positioned, so the invisible
	 * text spans line up with the raster underneath regardless of centering/zoom. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, viewport, 2, "div", "pageWrapper");
	page_wrapper = get_by_id("pageWrapper");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "position", "relative");
	/* margin-top/margin-bottom are recomputed per render for vertical centering and the
	 * box-shadow allowance respectively (see render_and_show); left/right stay auto here
	 * for horizontal centering, which doesn't need per-render recomputation. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-left", "auto");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "margin-right", "auto");
	/* Link in the same background-color: inherit chain as root/main_area/viewport (see
	 * main()) -- not visible behind img_page itself (opaque), but needed so #pageStatus
	 * below can inherit through it too. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_wrapper, 2, "background-color", "inherit");

	/* A real sibling block (in normal flow, after page_wrapper) rather than a margin-
	 * bottom on page_wrapper itself -- this WebKit fork doesn't count a scrolling
	 * container's last in-flow child's own trailing margin toward its scrollHeight at
	 * all (confirmed empirically: querying GET_SCROLL_GEOMETRY showed #viewport's
	 * scrollHeight exactly equal to page_wrapper's own offsetTop+offsetHeight, with
	 * margin-bottom contributing nothing whatsoever), so that margin was silently
	 * capped at zero for scrolling purposes no matter what value was set -- the page's
	 * bottom (and its shadow) could never actually be scrolled clear of the frame edge.
	 * A separate element's own height isn't ambiguous "trailing margin" in the same way,
	 * so it reliably extends scrollHeight instead. Resized to bottom_reserve per render
	 * (see render_and_show); page_wrapper's own margin-top continues to work fine and is
	 * unaffected by this (a leading margin on a scrolling container's content, not a
	 * trailing one, behaves differently in the same engine). */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, viewport, 2, "div", "bottomSpacer");
	bottom_spacer = get_by_id("bottomSpacer");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, bottom_spacer, 2, "width", "1px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, bottom_spacer, 2, "height", "0px");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "img", "page");
	img_page = get_by_id("page");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "background", "white");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "3px 3px 6px rgba(0,0,0,0.55), -6px -6px 5px rgba(255,255,255,0.2)");

	/* Experimental: an invisible, selectable text overlay in the Acrobat/PDF.js style --
	 * real text nodes positioned atop the raster image so the underlying content can be
	 * selected/copied, without needing DOM-level SVG rendering. Built per-render in
	 * update_text_layer() from PdfDocument::pageTextSpans(). Since Hipe has no client-side
	 * text-measurement API, spans can't be CSS-scaled to match glyph metrics exactly the
	 * way PDF.js does -- font-size is approximated from line height, so alignment is
	 * "good enough to select the right text", not pixel-perfect. */
	/* Overlay shown while a page is (re-)rendering or if that render fails -- covers
	 * img_page/text_layer (later in DOM order, plus an explicit z-index for safety) so
	 * the previous page's now-stale content isn't left on screen with no feedback during
	 * a slow render. Hidden by default; toggled via show_page_status()/hide_page_status(). */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "div", "pageStatus");
	page_status = get_by_id("pageStatus");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "z-index", "2");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "justify-content", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "text-align", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "white-space", "pre-line");
	/* Theme-matched (inherited, see page_wrapper/root above) rather than a hardcoded
	 * white panel with dark text -- that read as jarring/out of place against a dark
	 * theme. Since its fill now matches the surrounding chrome/viewport margin exactly,
	 * the dashed border below is what actually marks out its extent rather than being
	 * purely decorative; border-color is left unset so it defaults to currentColor,
	 * automatically matching whatever fg color came with the inherited theme. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "background-color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "border-width", "4px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "border-style", "dashed");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "font-size", "16px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "padding", "20px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "box-sizing", "border-box");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "div", "textLayer");
	text_layer = get_by_id("textLayer");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "overflow", "hidden");

	/* A child of #viewport (not #main_area) but position:fixed, so it floats over the
	 * document instead of occupying its own row that would shrink the scrollable area,
	 * "mile-high menubar" style. position:absolute was tried first and doesn't work for
	 * this: an absolutely-positioned descendant is still part of its containing block's
	 * own scrollable canvas, so it rode up the screen right along with the page content
	 * instead of staying put (confirmed live -- this WebKit fork also has no
	 * position:sticky, confirmed absent from hipecore's CSSValueKeywords.in, which would
	 * have been the more obvious tool otherwise). position:fixed anchors to the whole
	 * frame regardless of DOM nesting, which is also why "left" below is offset by
	 * SIDEBAR_WIDTH_PX rather than just 0 -- fixed ignores #viewport's own on-screen
	 * position entirely, so without this it would land at the frame's true left edge,
	 * underneath the sidebar, rather than at #viewport's.
	 *
	 * Docked at the TOP (not the bottom): a horizontal scrollbar (once zoomed in wider
	 * than the viewport) only ever renders along the bottom edge of a scrolling
	 * container, and Hipe's GET_GEOMETRY has no clientWidth/clientHeight equivalent (only
	 * offsetWidth/offsetHeight, which don't exclude scrollbars), so there was no reliable
	 * way to measure a real scrollbar's thickness and keep the toolbar clear of it down
	 * there. Docking at the top sidesteps the problem entirely rather than guessing a
	 * safety margin -- there's never a scrollbar up there to fight with, and the toolbar
	 * is nowhere near full width so it doesn't reach the vertical scrollbar on the right
	 * either. Flush against #viewport's own top-left corner (no margin/gap here, and
	 * border-radius dropped below) rather than floating with a gap like the old
	 * bottom-docked version did -- a rounded corner sitting hard against the real corner
	 * of the frame just looked wrong. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, viewport, 2, "div", "navbar");
	navbar = get_by_id("navbar");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "position", "fixed");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "top", "0");
	{
		char navbar_left_buf[16];
		snprintf(navbar_left_buf, sizeof(navbar_left_buf), "%dpx", (int) SIDEBAR_WIDTH_PX);
		hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "left", navbar_left_buf);
	}
	/* #pageStatus (the loading/error overlay, see main()) sets z-index:2 to sit above
	 * img_page/text_layer within #pageWrapper -- navbar has no competing z-index of its
	 * own by default, so despite being position:fixed it could still lose that stacking
	 * comparison and end up layered under the overlay instead of over it, same as it
	 * already correctly sits over ordinary scrolled page content. Comfortably higher
	 * than #pageStatus's 2 to make sure it wins. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "z-index", "10");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "padding", "4px 8px");
	/* Slight transparency now that it's an opaque theme-matched panel rather than the
	 * old rgba background -- opacity (not another background alpha) so it uniformly
	 * fades the whole toolbar, buttons/text included, rather than just the fill. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "opacity", "0.92");
	/* Matches body's theme color (see root in main()) instead of a hardcoded black, so it
	 * reads as one piece of chrome with sidebar and with periscope's own body-colormatched
	 * frame. A downward drop shadow (meeting sidebar's rightward one at their shared
	 * top-left corner) is what separates it from the document now, instead of the old
	 * flat semi-transparent panel look -- paired dark+light for the same reason as
	 * sidebar's (see its comment): a single-brightness shadow can disappear against a
	 * same-toned theme, so both are stacked (matching periscope's own tooltip/button
	 * technique in ~/periscope/hipe.css) to guarantee contrast on light or dark themes. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "background-color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "box-shadow", "0 2px 6px rgba(0,0,0,0.35), 0 2px 6px rgba(255,255,255,0.12)");
	/* Icon-first buttons (prev/next/fit) are only one or two glyphs wide -- a fixed
	 * min-width keeps them from shrinking to an uncomfortably small click target,
	 * and a larger font-size keeps the symbols themselves legible. Spacing between
	 * buttons/labels is done with explicit margin-right below rather than the flex
	 * "gap" property -- this WebKit fork only implements the older CSS
	 * multi-column "column-gap", not the flex/grid "gap" shorthand, so "gap"
	 * silently does nothing here (confirmed by checking RenderFlexibleBox.cpp in
	 * hipecore -- no gap handling at all). */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "#navbar button",
		"min-width:32px; padding:4px 8px; font-size:16px; cursor:pointer;");

	const char* GROUP_MARGIN = "22px";  /* between page nav / zoom / slideshow */
	const char* ITEM_MARGIN = "6px";    /* between controls within one group */

	/* Leftmost, ahead of the page-nav group -- now that sidebar and navbar read as one
	 * L-shaped panel (see #viewport/#navbar above), a sidebar show/hide toggle belongs
	 * with the rest of the chrome that panel represents rather than tucked away
	 * elsewhere. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "sidebarToggleBtn");
	hipe_loc sidebar_toggle_btn = get_by_id("sidebarToggleBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, sidebar_toggle_btn, 1, "\xe2\x98\xb0" /* ☰ */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar_toggle_btn, 2, "margin-right", GROUP_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "div", "pageGroup");
	hipe_loc page_group = get_by_id("pageGroup");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_group, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_group, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_group, 2, "margin-right", GROUP_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_group, 2, "button", "prevBtn");
	hipe_loc prev_btn = get_by_id("prevBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, prev_btn, 1, "\xe2\x97\x80" /* ◀ */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, prev_btn, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_group, 2, "span", "pageLabel");
	page_label = get_by_id("pageLabel");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_label, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_group, 2, "button", "nextBtn");
	hipe_loc next_btn = get_by_id("nextBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, next_btn, 1, "\xe2\x96\xb6" /* ▶ */);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "div", "zoomGroup");
	hipe_loc zoom_group = get_by_id("zoomGroup");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_group, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_group, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_group, 2, "margin-right", GROUP_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, zoom_group, 2, "button", "zoomOutBtn");
	hipe_loc zoom_out_btn = get_by_id("zoomOutBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, zoom_out_btn, 1, "\xf0\x9f\x94\x8d\xe2\x88\x92" /* 🔍− */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_out_btn, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, zoom_group, 2, "span", "zoomLabel");
	zoom_label = get_by_id("zoomLabel");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_label, 2, "margin-right", ITEM_MARGIN);
	/* Clicking the label itself resets to 100% zoom -- cursor:pointer as the only
	 * affordance for this (no tooltip support to spell it out otherwise). */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_label, 2, "cursor", "pointer");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, zoom_group, 2, "button", "zoomInBtn");
	hipe_loc zoom_in_btn = get_by_id("zoomInBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, zoom_in_btn, 1, "\xf0\x9f\x94\x8d+" /* 🔍+ */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, zoom_in_btn, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, zoom_group, 2, "button", "fitWidthBtn");
	hipe_loc fit_width_btn = get_by_id("fitWidthBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, fit_width_btn, 1, "\xf0\x9f\x94\x8d\xe2\x86\x94" /* 🔍↔ fit width */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, fit_width_btn, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, zoom_group, 2, "button", "fitPageBtn");
	hipe_loc fit_page_btn = get_by_id("fitPageBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, fit_page_btn, 1, "\xf0\x9f\x94\x8d\xf0\x9f\x93\x84" /* 🔍📄 fit page -- avoids ⛶, too easily read as periscope's own fullscreen icon */);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "div", "slideshowGroup");
	hipe_loc slideshow_group = get_by_id("slideshowGroup");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_group, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_group, 2, "align-items", "center");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, slideshow_group, 2, "button", "slideshowBtn");
	hipe_loc slideshow_btn = get_by_id("slideshowBtn");
	/* Kept as a word rather than an icon-only button -- it's a whole-mode switch,
	 * not a frequent nav action, and reusing "▶" here would clash with Next above.
	 * The desktop-computer glyph in front is just a visual cue, not a replacement
	 * for the label. */
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, slideshow_btn, 1, "\xf0\x9f\x92\xbb Slideshow" /* 💻 Slideshow */);

	/* Measured once now that navbar's real content/padding/font-size are all set -- used
	 * in render_and_show to reserve enough space above the page that scrolling to its top
	 * doesn't leave it hidden under the floating toolbar. Doesn't need remeasuring later:
	 * navbar's own height doesn't change across states (button/label text lengths vary
	 * but not the row height). */
	{
		float navbar_h = 0;
		get_geometry(navbar, nullptr, &navbar_h);
		navbar_clearance_px = navbar_h + 12.0f; /* + a little breathing room below it */
	}

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

	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SIDEBAR_TOGGLE, sidebar_toggle_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_PREV, prev_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_NEXT, next_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_OUT, zoom_out_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_IN, zoom_in_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_ZOOM_RESET, zoom_label, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_WIDTH, fit_width_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_FIT_PAGE, fit_page_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_ENTER, slideshow_btn, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_LEAVE, slideshow_leave_btn, 1, "click");
	/* REQ_SLIDESHOW_ADVANCE's "click" on main_area is requested/cancelled in
	 * enter_slideshow()/leave_slideshow() instead of unconditionally here -- see the
	 * comment there (matches "contextmenu"'s existing pattern, and for the same reason:
	 * registering it early would let the click that enters slideshow bubble up from the
	 * Slideshow button to main_area and immediately advance a page too). */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_KEYDOWN, 0, 1, "keydown"); /* location 0 = whole-frame keydown */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_WHEEL, viewport, 1, "wheel");
	/* "resize" is special-cased server-side to attach to the window regardless of the
	 * location given (see requestEvent() in hipecore's qwebelement.cpp), so location is
	 * arbitrary here -- 0 to match the other whole-frame requests above. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_RESIZE, 0, 1, "resize");

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
		/* Per the Hipe API docs: always check for a -1 return (disconnection) or an
		 * HIPE_OP_SERVER_DENIED opcode and exit -- otherwise an orphaned client (e.g.
		 * hiped restarting/crashing out from under it) spins forever, since a blocking
		 * call can no longer actually block on anything once disconnected. */
		if (hipe_next_instruction(session, &event, 1) < 0 || event.opcode == HIPE_OP_SERVER_DENIED)
			break;

		if (event.opcode == HIPE_OP_DIALOG_RETURN) {
			if (event.requestor == REQ_SLIDESHOW_DIALOG) handle_slideshow_dialog_return(event);
			continue;
		}

		if (event.opcode != HIPE_OP_EVENT) continue;
		/* requestor is only meaningful on HIPE_OP_EVENT replies to our own
		 * EVENT_REQUESTs -- other instruction types can carry unrelated
		 * requestor values that happen to collide with our REQ_* codes. */
		if (event.requestor == REQ_SIDEBAR_TOGGLE) toggle_sidebar();
		else if (event.requestor == REQ_PREV) render_and_show(current_page - 1, true);
		else if (event.requestor == REQ_NEXT) render_and_show(current_page + 1);
		else if (event.requestor == REQ_ZOOM_OUT) set_zoom(zoom_level / ZOOM_STEP);
		else if (event.requestor == REQ_ZOOM_IN) set_zoom(zoom_level * ZOOM_STEP);
		else if (event.requestor == REQ_ZOOM_RESET) set_zoom(1.0f);
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
		else if (event.requestor == REQ_WHEEL) handle_wheel_event(parse_wheel_delta_y(event.arg[1]));
		else if (event.requestor == REQ_RESIZE && fit_mode != FitMode::NONE) render_and_show(current_page);
		else if (event.requestor >= REQ_THUMB_BASE) render_and_show((int) (event.requestor - REQ_THUMB_BASE));
	} while (event.opcode != HIPE_OP_FRAME_CLOSE);

	hipe_close_session(session);
	return 0;
}
