#include <hipe.h>
#include "pdf_document.hpp"
#include "icon_data.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

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
#define REQ_SLIDESHOW_MOUSEMOVE 17
#define REQ_OPEN 18         /* "Open" toolbar button / empty-state panel click */
/* The requestor value passed with our HIPE_OP_FIFO_GET_PEER request; the server (top level)
 * or framing manager echoes it back on the HIPE_OP_FIFO_RESPONSE so we can recognise it.
 * Distinct from every other REQ_* -- though the dispatch only trusts it after checking
 * opcode == HIPE_OP_FIFO_RESPONSE, since an unrelated instruction could carry any requestor
 * (see the loop's own note about requestor collisions). */
#define REQ_FIFO_GET_PDF 19
#define REQ_THUMB_BASE 1000
/* Far above REQ_THUMB_BASE's own range (REQ_THUMB_BASE + page_count) so the two ranges
 * can never collide regardless of document length -- see update_link_layer/dispatch. */
#define REQ_LINK_BASE 100000

/* Name of the FIFO ability this app advertises to the framing manager (HIPE_OP_FIFO_ADD_ABILITY)
 * so another app -- e.g. a file shell doing "open with..." -- can push a PDF to it. Shown to
 * the user by the framing manager when picking a target; identifies the ability within Hipe
 * together with our client id. Not advertised in embedded mode. */
#define FIFO_HOST_ABILITY "Open"

/* Mouse cursors are applied as unicode characters via HIPE_OP_SET_CURSOR (the server
 * rasterises the glyph to an SVG cursor in the frame's fg/bg colours), not as CSS `cursor:`
 * keywords -- the native cursors those map to don't render on some embedded hosts. Matches
 * periscope's convention (its default is U+1F87C). U+1F87C is a NW-pointing arrow whose tip
 * sits at the server's fixed 0,7 hotspot; the busy cursor is a gear. The arrow is the
 * default almost everywhere (buttons and thumbnails included); the hand is reserved for the
 * two things that aren't obviously clickable from their appearance -- hyperlinks in the page
 * and the zoom-percent label. */
#define CURSOR_DEFAULT "\xf0\x9f\xa1\xbc" /* 🡼 U+1F87C -- NW arrow, matches server hotspot; the default */
#define CURSOR_POINTER "\xf0\x9f\x91\x86" /* 👆 U+1F446 -- hyperlinks and the clickable zoom label only */
#define CURSOR_TEXT    "\xe2\x8c\xb6"     /* ⌶ U+2336 -- over the selectable text overlay */
#define CURSOR_BUSY    "\xe2\x9a\x99"     /* ⚙ U+2699 -- during a slow page render */

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
/* Fallback used when a page times out at its intended resolution (see render_and_show).
 * Tuned empirically against a real stress-test file rather than assumed: probing a range
 * of widths against its worst (10s-timeout) page showed render time scaling roughly
 * LINEARLY with width for that page, not with pixel count/area as first assumed -- e.g.
 * 110px took ~3.9s, 200px ~6.4s, 300px ~10.3s, 350px ~13.1s, and 500px still hadn't
 * finished at 15s. That rules out pushing the fallback width very far up (350px, initially
 * chosen for legibility, actually needs MORE time than the original attempt's own 10s
 * budget for a page this bad) and rules out a short fallback timeout (even the 110px
 * thumbnail size needs several seconds here, well past an initially-assumed 3s). 200px
 * with a generous-over-its-measured-6.4s timeout is a size that's still meaningfully
 * bigger/more legible than THUMB_WIDTH_PX while leaving real margin for a page that scales
 * similarly, and still meaningfully shorter than the original budget for one that doesn't. */
static const float LOW_RES_FALLBACK_WIDTH_PX = 200.0f;
static const int LOW_RES_FALLBACK_TIMEOUT_MS = 8000;
/* A page's thumbnail render (see build_thumbnail_sidebar) taking this many times longer
 * than the batch's own median is treated as real evidence that page itself is unusually
 * complex, rather than the machine being transiently busy -- see page_is_flagged_complex.
 * 4x is deliberately generous: false positives just cost a smart_starting_width guess
 * instead of trying render_width first (still no timeout-budget penalty either way, see
 * render_and_show), so there's little downside to erring toward not flagging borderline
 * pages. */
static const float COMPLEXITY_FLAG_MULTIPLIER = 4.0f;
/* Target wall-clock budget (ms) smart_starting_width extrapolates a starting width
 * against -- under DEFAULT_RENDER_TIMEOUT_MS (10s) so the first attempt at a flagged
 * page's smart-chosen width still has some margin to land inside its budget even if the
 * linear-with-width extrapolation from a single small thumbnail data point is imperfect,
 * but deliberately close to it rather than conservatively far below: 10s was always the
 * accepted per-page wait (the original hard timeout), so a legible page that lands in 8-9s
 * is a better outcome than an artificially small one that returns quickly. If this proves
 * too aggressive in practice (flagged pages timing out instead of landing), the fallback
 * tier below still catches it -- just at the cost of a wasted first attempt. */
static const double SMART_WIDTH_TARGET_BUDGET_MS = 9000.0;
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
static hipe_loc link_layer;
static hipe_loc viewport;
static hipe_loc sidebar;
static hipe_loc navbar;
static hipe_loc main_area;
static hipe_loc empty_state;   /* "No document open" panel shown when launched with no file */
static hipe_loc open_btn;       /* "Open" toolbar button -- only created when launched with no file */
/* Path of the named pipe we (as FIFO host) created for an in-progress "open with..."
 * transfer, or empty. Tracked so a late/stray FIFO_CLOSE/FIFO_DROP_PEER and process exit
 * can unlink it. */
static std::string host_fifo_path;
/* Set when a FRAME_CLOSE or dropped server connection is seen from inside a blocking helper
 * (drain_fifo) rather than the main loop -- the loop checks it and exits. */
static bool g_should_exit = false;
static hipe_loc slideshow_controls; /* wraps slideshow_menu_btn + slideshow_leave_btn -- see enter_slideshow */
static hipe_loc slideshow_menu_btn;
static hipe_loc slideshow_leave_btn;
static hipe_loc page_label;
static hipe_loc zoom_label;
static float navbar_clearance_px = 0.0f; /* measured once after navbar is built -- see main() */
static float base_render_width = 800.0f;
static float zoom_level = 1.0f;
static float render_width = 800.0f;
static FitMode fit_mode = FitMode::NONE;
static bool slideshow_active = false;
static std::chrono::steady_clock::time_point slideshow_started_at; /* set in enter_slideshow() */
/* Reset whenever the current page actually changes (see render_and_show), regardless of
 * slideshow_active -- consumed by show_slideshow_dialog() to show how long the presenter
 * has lingered on the current slide, a real signal of "am I running long on this one" in
 * the middle of a talk. */
static std::chrono::steady_clock::time_point slide_shown_at;
static bool sidebar_visible = true;
static bool is_busy = false; /* true while a render_and_show call is blocked inside renderPagePng */
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
/* True when the raster currently on screen for current_page came from the low-res
 * fallback attempt (see render_and_show) rather than the originally requested
 * resolution -- reflected in the page label so it's clear the softness is a deliberate
 * degraded result, not a rendering bug. Reset alongside current_page_has_raster. */
static bool current_page_is_low_res = false;
static bool wheel_stuck = false;
static std::chrono::steady_clock::time_point wheel_stuck_since;
static std::chrono::steady_clock::time_point wheel_cooldown_until;
static std::chrono::steady_clock::time_point last_render_finished_at;
/* Guards reveal_slideshow_controls() against a trailing mousemove generated by the same
 * click/gesture that just caused hide_slideshow_controls() to run (e.g. a click-to-
 * advance) -- see hide_slideshow_controls(). */
static std::chrono::steady_clock::time_point slideshow_controls_reveal_cooldown_until;
/* Authoritative "is the Menu/Leave pair currently faded in" flag -- see
 * reveal_slideshow_controls()/hide_slideshow_controls()/the main loop's idle-fade poll. */
static bool slideshow_controls_visible = false;
static std::chrono::steady_clock::time_point slideshow_controls_last_shown_at;
static std::vector<hipe_loc> thumb_locs;
/* Per-page wall-clock time (ms) the eager thumbnail render took, and the median across
 * the whole document -- see build_thumbnail_sidebar/page_is_flagged_complex/
 * smart_starting_width. A page whose thumbnail took much longer than most others in the
 * SAME batch (same machine, same moment, so any transient system-wide lag affects all of
 * them roughly equally) is real evidence that page itself is unusually complex, not just
 * that the machine was busy right then. */
static std::vector<double> thumb_render_ms;
static double thumb_render_ms_median = 0.0;
/* Resolved click target for each link overlay element currently in #linkLayer, indexed by
 * (event.requestor - REQ_LINK_BASE) -- see update_link_layer/the event dispatch loop.
 * Rebuilt from scratch on every render_and_show call, same lifecycle as the elements
 * themselves (cleared and recreated alongside #linkLayer's contents). */
struct ResolvedLink {
	bool is_external;
	std::string uri; /* only meaningful when is_external */
	int target_page; /* only meaningful when !is_external */
};
static std::vector<ResolvedLink> current_page_links;

/* Opt-in tracing for the FIFO import/export handshakes (client and host), which run against
 * a framing manager and so can't be stepped through here. Set HIPE_PDF_DEBUG=1 to see how
 * far an exchange gets on stderr. */
static bool debug_enabled() {
	static int v = -1;
	if (v < 0) {
		const char* e = getenv("HIPE_PDF_DEBUG");
		v = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	return v;
}
#define DBG(...) do { if (debug_enabled()) { fprintf(stderr, "hipe-pdf: " __VA_ARGS__); fflush(stderr); } } while (0)

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
	char buf[80];
	if (current_page_is_low_res)
		snprintf(buf, sizeof(buf), "Page %d / %d (low-res)", current_page + 1, page_count);
	else
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

static bool page_is_flagged_complex(int page_number) {
	if (page_number < 0 || page_number >= (int) thumb_render_ms.size()) return false;
	if (thumb_render_ms_median <= 0.0) return false; /* no data (e.g. thumbnails not built yet) */
	return thumb_render_ms[page_number] > thumb_render_ms_median * COMPLEXITY_FLAG_MULTIPLIER;
}

static float smart_starting_width(int page_number, float intended_width) {
	/* Extrapolates a starting width from the page's own thumbnail time via the linear
	 * width/time relationship measured empirically for pathological pages (see
	 * LOW_RES_FALLBACK_WIDTH_PX's comment) -- i.e. assumes render_ms is roughly
	 * proportional to width for a given page, so scaling THUMB_WIDTH_PX by the ratio of
	 * the target budget to the thumbnail's own measured time estimates the width that
	 * would take about that long. Clamped so a flagged page never gets a wider first
	 * attempt than it would've had anyway (intended_width), nor a smaller one than the
	 * separate low-res fallback tier already tries (no point in a "smart" width that's
	 * just a worse version of that safety net). */
	double thumb_ms = thumb_render_ms[page_number];
	if (thumb_ms <= 0.0) return intended_width;
	float estimated = THUMB_WIDTH_PX * (float) (SMART_WIDTH_TARGET_BUDGET_MS / thumb_ms);
	return std::max(LOW_RES_FALLBACK_WIDTH_PX, std::min(intended_width, estimated));
}

/* Which thumbnail currently wears the highlight border. File-scope (not a function-local
 * static) so swap_in_document() can reset it to -1 when a new document replaces the sidebar
 * -- otherwise a stale index could clear the border on the wrong page's fresh thumbnail. */
static int highlighted_thumb = -1;

static void highlight_thumbnail(int page_number) {
	if (highlighted_thumb >= 0 && highlighted_thumb < (int) thumb_locs.size())
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb_locs[highlighted_thumb], 2, "border", "2px solid transparent");
	if (page_number >= 0 && page_number < (int) thumb_locs.size())
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb_locs[page_number], 2, "border", "2px solid #3388ff");
	highlighted_thumb = page_number;
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

	/* background-color longhand, not the "background" shorthand: the shorthand also wipes
	 * background-image etc., so clearing it in leave_slideshow can't put #main_area's
	 * original "background-color: inherit" back (nothing in any stylesheet targets
	 * #main_area to fall back to), leaving it -- and #viewport/#navbar, which inherit from
	 * it -- transparent. Setting just the longhand keeps leave_slideshow's restore simple. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background-color", color_buf);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "background-color", color_buf);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "color", fg);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_leave_btn, 2, "color", fg);
}

static void update_text_layer(int page_number, float scale) {
	hipe_send(session, HIPE_OP_CLEAR, 0, text_layer, 0);

	/* No selectable-text overlay during slideshow. Each span has pointer-events:auto (so a
	 * drag can select it), and it sits between the raster and the click-to-advance listener
	 * on img_page -- a click that lands on page text is caught by the span and, being on a
	 * sibling of img_page rather than an ancestor, never bubbles to that listener, so it
	 * silently selected text instead of advancing the slide. Cleared here and left empty
	 * while presenting; leave_slideshow()'s re-render rebuilds it. Hyperlinks still work --
	 * they're a separate overlay (#linkLayer, see update_link_layer). */
	if (slideshow_active) return;

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

static void update_link_layer(int page_number, float scale) {
	hipe_send(session, HIPE_OP_CLEAR, 0, link_layer, 0);
	current_page_links.clear();

	std::vector<PdfDocument::PageLink> links;
	try {
		links = doc->pageLinks(page_number);
	} catch (const std::exception& e) {
		fprintf(stderr, "update_link_layer: %s\n", e.what());
		return;
	}

	char buf[16];
	for (const auto& link : links) {
		/* Skip anything unresolvable up front (an internal link MuPDF couldn't map to a
		 * page) rather than register a click target that would silently do nothing --
		 * both the DOM element and current_page_links stay in lockstep this way, indexed
		 * by (event.requestor - REQ_LINK_BASE) without gaps. */
		if (!link.is_external && link.target_page < 0) continue;

		hipe_send(session, HIPE_OP_APPEND_TAG, 0, link_layer, 1, "div");
		hipe_loc loc = hipe_newest_location();

		/* Common properties (position:absolute, pointer-events:auto, ...) come from the
		 * "#linkLayer div" rule added once at startup, and the CURSOR_POINTER glyph is
		 * inherited from #linkLayer (see main()); only per-link geometry and the click
		 * registration need setting here. */
		snprintf(buf, sizeof(buf), "%dpx", (int) (link.x * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "left", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (link.y * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "top", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (link.width * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "width", buf);
		snprintf(buf, sizeof(buf), "%dpx", (int) (link.height * scale));
		hipe_send(session, HIPE_OP_SET_STYLE, 0, loc, 2, "height", buf);

		int requestor = REQ_LINK_BASE + (int) current_page_links.size();
		hipe_send(session, HIPE_OP_EVENT_REQUEST, requestor, loc, 1, "click");

		ResolvedLink resolved;
		resolved.is_external = link.is_external;
		resolved.uri = link.uri;
		resolved.target_page = link.target_page;
		current_page_links.push_back(std::move(resolved));
	}
}

static void show_page_status(const char* message) {
	hipe_send(session, HIPE_OP_SET_TEXT, 0, page_status, 1, message);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "display", "flex");
}

static void hide_page_status() {
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "display", "none");
}

static const auto SLIDESHOW_CONTROLS_REVEAL_COOLDOWN = std::chrono::milliseconds(400);
/* How long the pair stays visible with no mouse movement before fading itself back out --
 * the classic PowerPoint presenter-view behavior this whole feature is modeled on. Checked
 * by the main loop's idle-poll while slideshow_active (see main()). */
static const auto SLIDESHOW_CONTROLS_IDLE_TIMEOUT = std::chrono::milliseconds(3000);

static void reveal_slideshow_controls() {
	if (std::chrono::steady_clock::now() < slideshow_controls_reveal_cooldown_until) return;
	/* Refresh the idle clock on every real movement, not just the first one after a hide
	 * -- this is what actually keeps the pair up while the mouse keeps moving. */
	slideshow_controls_last_shown_at = std::chrono::steady_clock::now();
	if (slideshow_controls_visible) return;
	slideshow_controls_visible = true;
	/* opacity/pointer-events, not display -- display:none can't be CSS-transitioned, and
	 * this is meant to fade, not snap. #slideshowControls stays display:flex permanently
	 * (see its DOM setup); pointer-events:none while faded out keeps a technically-still-
	 * present-but-invisible container from intercepting clicks meant for the slide
	 * beneath it (same reasoning as #textLayer/#linkLayer's own pointer-events handling). */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "opacity", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "pointer-events", "auto");
}

static void hide_slideshow_controls() {
	/* Brief cooldown before the pair can be revealed again -- guards against a trailing
	 * mousemove generated by the very click/gesture that caused this hide (e.g. a click-
	 * to-advance) immediately re-revealing what was just intentionally hidden. Confirmed
	 * live this was a real problem, not theoretical: without it, a plain click-to-advance
	 * left the Menu/Leave pair visible again right after, despite the hide call itself
	 * definitely having run. */
	slideshow_controls_reveal_cooldown_until = std::chrono::steady_clock::now() + SLIDESHOW_CONTROLS_REVEAL_COOLDOWN;
	slideshow_controls_visible = false;
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "opacity", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "pointer-events", "none");
}

static void check_slideshow_controls_idle_timeout() {
	if (!slideshow_controls_visible) return;
	if (std::chrono::steady_clock::now() - slideshow_controls_last_shown_at >= SLIDESHOW_CONTROLS_IDLE_TIMEOUT)
		hide_slideshow_controls();
}

static void render_and_show(int page_number, bool land_at_bottom = false) {
	if (page_number < 0 || page_number >= page_count) return;
	bool page_changed = (page_number != current_page);
	int previous_page = current_page; /* for a less-jarring loading-placeholder color below */
	current_page = page_number;
	if (page_changed) {
		current_page_has_raster = false; /* whatever img_page shows now is for a different page */
		current_page_is_low_res = false;
	}

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
		/* Drop the old page's text spans and link targets now rather than leaving them
		 * selectable/clickable underneath the overlay. */
		hipe_send(session, HIPE_OP_CLEAR, 0, text_layer, 0);
		hipe_send(session, HIPE_OP_CLEAR, 0, link_layer, 0);
		current_page_links.clear();

		/* Colored to match the page just being left (previous_page -- on the very first
		 * ever render this is current_page itself, which works out fine too: sampling
		 * the incoming page's own likely background beats a generic theme color even
		 * then) rather than the theme-inherited color used elsewhere in #pageStatus --
		 * a plain blank page in roughly the right tone reads as much less jarring mid-
		 * navigation than a flash of theme chrome. Same luminance-based contrast pick as
		 * update_slideshow_background so the message text stays legible either way. */
		uint8_t bg_r = 255, bg_g = 255, bg_b = 255;
		doc->pageBackgroundColor(previous_page, &bg_r, &bg_g, &bg_b);
		char bg_buf[24];
		snprintf(bg_buf, sizeof(bg_buf), "rgb(%d,%d,%d)", bg_r, bg_g, bg_b);
		hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "background-color", bg_buf);
		double luminance = 0.299 * bg_r + 0.587 * bg_g + 0.114 * bg_b;
		hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "color", luminance > 128.0 ? "black" : "white");

		char loading_buf[64];
		snprintf(loading_buf, sizeof(loading_buf), "Loading %s %d / %d...",
			slideshow_active ? "slide" : "page", current_page + 1, page_count);
		show_page_status(loading_buf);
		update_page_label();
		highlight_thumbnail(current_page);
	}

	is_busy = true;
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, 0, 1, CURSOR_BUSY);
	std::vector<uint8_t> png;
	bool low_res_fallback = false;
	/* A page whose own thumbnail render took far longer than the batch's median (see
	 * page_is_flagged_complex) gets a smaller, evidence-based first-attempt width instead
	 * of render_width -- still tried with the full default timeout it would've gotten
	 * anyway, so this never costs a flagged page any patience it would otherwise have
	 * had, only gives it a real shot at succeeding directly instead of predictably
	 * burning the whole budget failing at render_width first. Unflagged pages are
	 * completely unaffected: first_attempt_width just equals render_width for them. */
	bool complex_flag = page_is_flagged_complex(current_page);
	float first_attempt_width = complex_flag ? smart_starting_width(current_page, render_width) : render_width;
	try {
		png = doc->renderPagePng(current_page, first_attempt_width);
		if (complex_flag && first_attempt_width < render_width) low_res_fallback = true;
	} catch (const std::exception& e) {
		fprintf(stderr, "Failed to render page %d at %.0fpx: %s\n", current_page, first_attempt_width, e.what());
		/* A much smaller raster from the same pathological page is often (not always)
		 * tractable well within a fraction of the original budget: rasterization/AA cost
		 * scales with pixel count, and plenty of real pathological pages turn out to be
		 * bound by that rather than by content-stream complexity -- confirmed
		 * empirically (a stress-test file's own sidebar thumbnail, rendered at
		 * THUMB_WIDTH_PX, succeeded on a page whose full-resolution render timed out).
		 * Worth one more, cheaper attempt before giving up outright. The shorter
		 * LOW_RES_FALLBACK_TIMEOUT_MS reflects that: if this doesn't finish quickly, it's
		 * not going to, and there's no reason to make the user wait a second full budget. */
		try {
			png = doc->renderPagePng(current_page, LOW_RES_FALLBACK_WIDTH_PX, LOW_RES_FALLBACK_TIMEOUT_MS);
			low_res_fallback = true;
		} catch (const std::exception& e2) {
			is_busy = false;
			hipe_send(session, HIPE_OP_SET_CURSOR, 0, 0, 1, CURSOR_DEFAULT);
			fprintf(stderr, "Low-res fallback for page %d also failed: %s\n", current_page, e2.what());
			char err_buf[128];
			snprintf(err_buf, sizeof(err_buf),
				"%s %d could not be rendered\n(ludicrously complex, or timed out)",
				slideshow_active ? "Slide" : "Page", current_page + 1);
			/* Back to the theme-inherited look (see main()) for the error state specifically --
			 * unlike the loading placeholder, an error is deliberately distinct chrome rather
			 * than something that should blend in with the page content. */
			hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "background-color", "inherit");
			hipe_send(session, HIPE_OP_SET_STYLE, 0, page_status, 2, "color", "inherit");
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
			if (page_changed) {
				slide_shown_at = last_render_finished_at;
				if (slideshow_active) hide_slideshow_controls();
				hipe_send(session, HIPE_OP_SCROLL_TO, 0, viewport, 3, (char*) nullptr, "0", "%");
			}
			return;
		}
	}
	current_page_is_low_res = low_res_fallback;
	is_busy = false;
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, 0, 1, CURSOR_DEFAULT);
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
	update_link_layer(current_page, render_width / (page_w > 0 ? page_w : render_width));

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
	if (page_changed) {
		slide_shown_at = last_render_finished_at;
		if (slideshow_active) hide_slideshow_controls();
		hipe_send(session, HIPE_OP_SCROLL_TO, 0, viewport, 3, (char*) nullptr, land_at_bottom ? "100" : "0", "%");
	}
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
	/* Both reset here, not just slideshow_started_at: whatever page happens to already be
	 * on screen becomes "freshly shown" the moment the presenter actually starts
	 * presenting, regardless of how long it had been sitting there during casual
	 * browsing beforehand. Also covers a real edge case: render_and_show only resets
	 * slide_shown_at when the page number actually changes, which doesn't happen if
	 * slideshow is entered while already on the same page as the app's very first
	 * (startup) render -- without this, that leaves slide_shown_at at its
	 * default-constructed epoch, showing a nonsense multi-decade "time on this slide". */
	slideshow_started_at = std::chrono::steady_clock::now();
	slide_shown_at = slideshow_started_at;
	saved_fit_mode = fit_mode;
	saved_zoom_level = zoom_level;

	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "none");
	/* Starts hidden, not shown -- the Menu/Leave button pair only appears once the
	 * presenter actually moves the mouse (see REQ_SLIDESHOW_MOUSEMOVE below), a deliberate
	 * PowerPoint-style "don't clutter the slide until asked" choice rather than being
	 * permanently on screen throughout the whole presentation. */
	hide_slideshow_controls();
	/* Dropped so the page blends into the color-matched surround instead of standing
	 * out inside a boxed frame -- update_slideshow_background() takes over from here. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "none");
	/* Requesting "contextmenu" always forces preventDefault on the server side (not
	 * something our request controls), suppressing the native menu -- including its
	 * Copy item for a text selection. So this is only requested while actually in
	 * slideshow, not unconditionally at startup, so text stays copyable otherwise. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_MENU, main_area, 1, "contextmenu");
	/* On img_page specifically, not main_area -- slideshow_controls (the Menu/Leave pair's
	 * container) is a direct child of main_area, a sibling of #viewport (which contains
	 * img_page several levels down: main_area > viewport > pageWrapper > img_page). A
	 * click on either button bubbles up through main_area but never passes through
	 * img_page's own branch of the tree, so it can no longer register as a stray page-
	 * advance. Confirmed live this actually mattered: with the listener on main_area
	 * (which both branches share as their common ancestor), clicking the Menu button also
	 * silently advanced to the next slide underneath the open dialog. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_ADVANCE, img_page, 1, "click");
	/* Reveals the Menu/Leave pair -- see reveal_slideshow_controls()/the dispatch loop.
	 * No separate touch-specific event is requested: this assumes (not independently
	 * verified against real touch hardware, only tested here with a mouse) that a tap
	 * surfaces as a synthesized mousemove/click the way it does in most WebKit-derived
	 * engines. If that turns out not to hold on the actual target hardware, a touch
	 * presenter would need a dedicated "touchstart" request added here instead/as well. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_MOUSEMOVE, main_area, 1, "mousemove");

	set_fit_mode(FitMode::PAGE); /* triggers render_and_show, which calls update_slideshow_background */
}

static void leave_slideshow() {
	slideshow_active = false;

	/* Respects sidebar_visible rather than forcing it back on -- a user who'd toggled the
	 * sidebar off before entering slideshow shouldn't have that choice silently undone by
	 * leaving it again. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, sidebar, 2, "display", sidebar_visible ? "block" : "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, navbar, 2, "display", "flex");
	hide_slideshow_controls();
	hipe_send(session, HIPE_OP_SET_STYLE, 0, img_page, 2, "box-shadow", "3px 3px 6px rgba(0,0,0,0.55), -6px -6px 5px rgba(255,255,255,0.2)");
	/* #main_area is restored to its main()-set "background-color: inherit" explicitly (no
	 * stylesheet rule targets it, so an empty value would strand it at transparent and
	 * take #viewport/#navbar, which inherit from it, down with it). #body has a theme rule
	 * to fall back to, so clearing its inline override is enough. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "background-color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "background-color", "");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, /*body*/ 0, 2, "color", "");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, main_area, 1, "contextmenu");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, img_page, 1, "click");
	hipe_send(session, HIPE_OP_EVENT_CANCEL, 0, main_area, 1, "mousemove");

	if (saved_fit_mode == FitMode::NONE) set_zoom(saved_zoom_level);
	else set_fit_mode(saved_fit_mode);
}

static void format_duration(std::chrono::steady_clock::duration d, char* buf, size_t buf_size) {
	long total_seconds = (long) std::chrono::duration_cast<std::chrono::seconds>(d).count();
	if (total_seconds < 0) total_seconds = 0; /* clock skew safety net, shouldn't happen with steady_clock */
	long h = total_seconds / 3600;
	long m = (total_seconds % 3600) / 60;
	long s = total_seconds % 60;
	if (h > 0) snprintf(buf, buf_size, "%ld:%02ld:%02ld", h, m, s);
	else snprintf(buf, buf_size, "%ld:%02ld", m, s);
}

static void show_slideshow_dialog() {
	/* Replaces a static "Choose an action:" prompt with live info that actually matters
	 * mid-presentation: where you are in the deck, the wall-clock time (for a hard
	 * finish-by deadline), how long the whole talk has run, and how long you've lingered
	 * on THIS slide specifically -- a real "am I running long on this one" signal that a
	 * presenter can't otherwise see once slideshow mode has hidden all normal chrome. */
	auto now = std::chrono::steady_clock::now();
	char slideshow_elapsed_buf[16], slide_elapsed_buf[16], clock_buf[16], prompt_buf[160];
	format_duration(now - slideshow_started_at, slideshow_elapsed_buf, sizeof(slideshow_elapsed_buf));
	format_duration(now - slide_shown_at, slide_elapsed_buf, sizeof(slide_elapsed_buf));

	time_t t = time(nullptr);
	struct tm local_tm;
	localtime_r(&t, &local_tm);
	strftime(clock_buf, sizeof(clock_buf), "%I:%M %p", &local_tm);

	/* Clock on its own line rather than crammed alongside the slide count -- the previous
	 * "Slide X of Y  ·  clock" layout looked cramped in practice (periscope's dialog
	 * renders this as plain text, where repeated literal spaces collapse the same way
	 * HTML text does, so padding with extra spaces didn't actually add visual room; a
	 * real newline is the only reliable way to separate them). */
	snprintf(prompt_buf, sizeof(prompt_buf),
		"Slide %d of %d\n%s\nTotal time: %s  \xc2\xb7  This slide: %s",
		current_page + 1, page_count, clock_buf, slideshow_elapsed_buf, slide_elapsed_buf);

	/* arg[3] symbols line up 1:1 with the arg[2] choices, plus one trailing symbol for
	 * the dialog itself: prev=U+23F4 \xe2\x8f\xb4, next=U+23F5 \xe2\x8f\xb5 (same media-
	 * control glyph family as the Menu button's U+23F6, and matching the toolbar's own
	 * prev/next buttons -- see prev_btn/next_btn below), start=\xe2\x8f\xae,
	 * end=\xe2\x8f\xad, leave=\xe2\x9c\x95, dialog icon=laptop (\xf0\x9f\x92\xbb) --
	 * matches the toolbar's own Slideshow button glyph rather than reusing "next"'s
	 * triangle for the dialog itself. */
	hipe_send(session, HIPE_OP_DIALOG, REQ_SLIDESHOW_DIALOG, 0, 4,
		"Slideshow", prompt_buf,
		"Previous page\nNext page\nGo to start\nGo to end\nLeave slideshow",
		"\xe2\x8f\xb4\n\xe2\x8f\xb5\n\xe2\x8f\xae\n\xe2\x8f\xad\n\xe2\x9c\x95\n\xf0\x9f\x92\xbb");
}

static void handle_slideshow_dialog_return(const hipe_instruction& reply) {
	/* Resolving the menu (picking a choice, or cancelling it) counts as "the next action"
	 * for the Menu/Leave pair's own show-on-move/hide-on-next-action lifecycle, same as an
	 * actual page change -- covers cases 1-4 below where the requested page change is a
	 * no-op (e.g. "Previous page" already on slide 1) and so wouldn't otherwise trigger
	 * render_and_show's own page_changed-gated hide. Redundant with (but harmless
	 * alongside) the hides already reached via render_and_show/leave_slideshow for the
	 * cases where the page genuinely does change. */
	if (slideshow_active) hide_slideshow_controls();

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
	thumb_render_ms.reserve(page_count);
	for (int i = 0; i < page_count; i++) {
		std::vector<uint8_t> thumb_png;
		auto render_start = std::chrono::steady_clock::now();
		try {
			thumb_png = doc->renderPagePng(i, THUMB_WIDTH_PX);
		} catch (const std::exception& e) {
			/* Still record the elapsed time even on failure/timeout -- a thumbnail that
			 * timed out is itself the strongest possible complexity signal (it already
			 * took the full DEFAULT_RENDER_TIMEOUT_MS at the smallest size we ever
			 * render), not a lack of data. */
			thumb_render_ms.push_back(std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - render_start).count());
			fprintf(stderr, "Failed to render thumbnail %d: %s\n", i, e.what());
			thumb_locs.push_back(0);
			continue;
		}
		thumb_render_ms.push_back(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - render_start).count());

		hipe_send(session, HIPE_OP_APPEND_TAG, 0, sidebar, 1, "img");
		hipe_loc thumb = hipe_newest_location();
		thumb_locs.push_back(thumb);

		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "display", "block");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "width", "110px");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "margin", "6px auto");
		hipe_send(session, HIPE_OP_SET_STYLE, 0, thumb, 2, "border", "2px solid transparent");
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

	/* Median rather than mean so a handful of genuinely pathological pages (which is
	 * exactly what this table exists to find) can't drag the baseline up and mask each
	 * other -- see page_is_flagged_complex. */
	if (!thumb_render_ms.empty()) {
		std::vector<double> sorted = thumb_render_ms;
		std::sort(sorted.begin(), sorted.end());
		/* Lower-middle element (not upper) for an even count: with very few pages -- a
		 * 2-page document is a real, common case -- the upper-middle element of a sorted
		 * pair IS the larger value, so a single outlier page would BE its own "median"
		 * and could never exceed COMPLEXITY_FLAG_MULTIPLIER times itself. Confirmed via
		 * live testing against a real 2-page stress-test file: page 2's genuinely
		 * pathological page went undetected until switching to this convention. */
		thumb_render_ms_median = sorted[(sorted.size() - 1) / 2];
	}
}

/* ---- Opening a document at runtime (see the "Open" toolbar button) ---------------------
 *
 * When launched with no file argument the app starts document-less, showing #emptyState and
 * an "Open" button. The button asks the environment for a PDF through Hipe's FIFO framework
 * (HIPE_OP_FIFO_GET_PEER): at top level Hipe answers with a native file dialog and hands back
 * a real filesystem path; under a framing manager (periscope) the manager mediates a FIFO
 * peer and hands back a named pipe to stream the bytes through. Either way the answer is a
 * HIPE_OP_FIFO_RESPONSE processed in the main loop -> handle_fifo_response() below.
 */

static void show_empty_state() {
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "display", "flex");
}

static void hide_empty_state() {
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "display", "none");
}

/* Everything that has to happen when a freshly parsed document takes over the screen --
 * whether from nothing (startup with a file arg) or replacing a document already open (a
 * runtime Open). Tears down the previous document's sidebar and per-document caches first,
 * so it's safe to call repeatedly. new_doc must have at least one page (checked by the
 * caller). */
static void swap_in_document(std::unique_ptr<PdfDocument> new_doc, const std::string& display_name) {
	doc = std::move(new_doc);
	page_count = doc->pageCount();
	current_page = 0;

	/* Back to defaults: carrying a previous document's zoom/fit state onto an unrelated new
	 * one (with entirely different page dimensions) is more surprising than helpful. */
	zoom_level = 1.0f;
	fit_mode = FitMode::NONE;
	current_page_has_raster = false;
	current_page_is_low_res = false;
	current_page_links.clear();

	/* Drop the previous document's sidebar plus the per-document complexity table that
	 * page_is_flagged_complex()/smart_starting_width() read from (those two vectors + the
	 * median are the whole cache). highlighted_thumb is reset so a stale index can't clear
	 * the border on one of the incoming thumbnails. */
	hipe_send(session, HIPE_OP_CLEAR, 0, sidebar, 0);
	thumb_locs.clear();
	thumb_render_ms.clear();
	thumb_render_ms_median = 0.0;
	highlighted_thumb = -1;

	hide_empty_state();

	if (!display_name.empty()) {
		std::string title = "hipe-pdf \xe2\x80\x94 " + display_name;
		hipe_send(session, HIPE_OP_SET_TITLE, 0, 0, 1, title.c_str());
	}

	/* Same initial sizing main() uses: fill ~90% of the content area. */
	float main_w = 0, main_h = 0;
	get_geometry(main_area, &main_w, &main_h);
	if (main_w > 100) base_render_width = main_w * 0.9f;
	render_width = base_render_width * zoom_level;

	build_thumbnail_sidebar(sidebar);
	render_and_show(0);
	update_zoom_label();
}

/* Validates a just-constructed document and either swaps it in or reports why it can't be.
 * Used for runtime opens; startup keeps its own fail-fast path in main(). */
static void accept_document(std::unique_ptr<PdfDocument> new_doc, const std::string& display_name) {
	int pages = 0;
	try {
		pages = new_doc->pageCount();
	} catch (const std::exception& e) {
		fprintf(stderr, "open: %s\n", e.what());
	}
	if (pages <= 0) {
		fprintf(stderr, "open: '%s' has no pages\n", display_name.c_str());
		if (!doc) show_empty_state(); /* else leave the current document on screen */
		return;
	}
	swap_in_document(std::move(new_doc), display_name);
}

static void request_open_document() {
	/* arg0: suggested name without extension; arg1: minimal required access mode (read);
	 * arg2: caption line then newline-separated "ext:description" type filters. We don't
	 * block waiting for the reply here -- it comes back asynchronously as a
	 * HIPE_OP_FIFO_RESPONSE (see the main loop) so the UI stays responsive while the file
	 * dialog / peer picker is open. */
	hipe_send(session, HIPE_OP_FIFO_GET_PEER, REQ_FIFO_GET_PDF, 0, 3,
		"document", "r",
		"Open PDF document\npdf:Portable Document Format");
}

/* Reads a non-blocking fd to EOF into a buffer, polling so a stalled peer can't hang us
 * forever (30s cap). Shared by both FIFO roles -- the client reader (read_fifo_resource)
 * and the host reader (handle_incoming_fifo_get_peer). `who` just labels log messages.
 *
 * End of transfer is either a real pipe EOF (peer closed its write fd) OR a FIFO_CLOSE /
 * FIFO_DROP_PEER instruction from the peer for `fifo_path`. The file shell's exporter keeps
 * its write fd open and signals completion with the instruction rather than an EOF (see
 * ~/export/export.cpp), so a pure pipe-poll loop would stall here until the 30s cap -- hence
 * the hipe socket is also pumped, non-blocking, each iteration. A FRAME_CLOSE or lost server
 * connection mid-transfer sets g_should_exit and ends the drain; any other instruction that
 * lands during the transfer window is dropped (logged under HIPE_PDF_DEBUG). */
static std::vector<uint8_t> drain_fifo(int fd, const char* fifo_path, const char* who) {
	std::vector<uint8_t> bytes;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	uint8_t chunk[64 * 1024];
	bool eof = false, peer_closed = false;
	hipe_instruction tmp;
	hipe_instruction_init(&tmp);

	while (!eof && !peer_closed) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int pr = poll(&pfd, 1, 200);
		if (pr < 0 && errno != EINTR) {
			fprintf(stderr, "%s: poll: %s\n", who, strerror(errno));
			break;
		}
		if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
			ssize_t n;
			while ((n = read(fd, chunk, sizeof(chunk))) > 0)
				bytes.insert(bytes.end(), chunk, chunk + n);
			if (n == 0) eof = true;
			else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				fprintf(stderr, "%s: read: %s\n", who, strerror(errno));
				break;
			}
		}
		short got;
		while (!peer_closed && (got = hipe_next_instruction(session, &tmp, 0)) != 0) {
			if (got < 0) { g_should_exit = true; peer_closed = true; break; }
			bool close_sig = (tmp.opcode == HIPE_OP_FIFO_CLOSE || tmp.opcode == HIPE_OP_FIFO_DROP_PEER)
				&& tmp.arg[0] && fifo_path && strcmp(tmp.arg[0], fifo_path) == 0;
			if (close_sig) {
				peer_closed = true;
			} else if (tmp.opcode == HIPE_OP_FRAME_CLOSE) {
				g_should_exit = true;
				peer_closed = true;
			} else {
				DBG("%s: dropped instruction opcode=%d during transfer\n", who, (int) tmp.opcode);
			}
		}
		if (!eof && !peer_closed && std::chrono::steady_clock::now() > deadline) {
			fprintf(stderr, "%s: timed out after 30s (%zu bytes so far)\n", who, bytes.size());
			break;
		}
	}
	hipe_instruction_clear(&tmp);

	/* On a FIFO_CLOSE signal the last bytes are already in the pipe buffer (the peer wrote
	 * everything before sending it, and the instruction crosses the relay slower than the
	 * bytes cross the kernel) -- sweep whatever is readable now. */
	if (peer_closed && !g_should_exit) {
		ssize_t n;
		while ((n = read(fd, chunk, sizeof(chunk))) > 0)
			bytes.insert(bytes.end(), chunk, chunk + n);
	}
	return bytes;
}

/* Reads a whole FIFO resource (named pipe) into memory as the FIFO *client*, running the
 * client side of the HIPE_OP_FIFO_OPEN handshake. Returns the bytes, or an empty vector on
 * any failure.
 *
 * Synchronous, like the other request/response exchanges in this file (get_geometry() etc.):
 * a slow mediated transfer blocks the client the same way a slow page render already does.
 * Known gap: the FIFO_OPEN echo below depends on the framing manager relaying the
 * instruction in both directions. A manager that answers FIFO_GET_PEER but never relays the
 * open handshake would leave hipe_await_instruction() blocked here. periscope -- the only
 * framing manager this runs under -- is expected to implement it; if that proves fragile
 * this should move into the main loop as a small state machine. */
static std::vector<uint8_t> read_fifo_resource(const char* fifo_path) {
	/* Reader opens first, non-blocking so open() succeeds even though no writer has
	 * attached yet (a blocking open would deadlock: the host only opens its write end in
	 * response to the FIFO_OPEN we send just below). */
	int fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "read_fifo_resource: open('%s'): %s\n", fifo_path, strerror(errno));
		return {};
	}

	DBG("client: pipe %s open, sent FIFO_OPEN, awaiting host echo\n", fifo_path);
	hipe_send(session, HIPE_OP_FIFO_OPEN, 0, 0, 2, fifo_path, "r");

	hipe_instruction ack;
	hipe_instruction_init(&ack);
	hipe_await_instruction(session, &ack, HIPE_OP_FIFO_OPEN); /* host's "my write end is open" echo */
	hipe_instruction_clear(&ack);
	DBG("client: got host FIFO_OPEN echo, draining\n");

	std::vector<uint8_t> bytes = drain_fifo(fd, fifo_path, "read_fifo_resource");
	close(fd);
	DBG("client: drained %zu bytes\n", bytes.size());

	/* Tell the host we're done: close this transfer, then drop the peer entirely (a viewer
	 * only ever reads the file once -- there's no save-back). */
	hipe_send(session, HIPE_OP_FIFO_CLOSE, 0, 0, 1, fifo_path);
	hipe_send(session, HIPE_OP_FIFO_DROP_PEER, 0, 0, 1, fifo_path);
	return bytes;
}

static void handle_fifo_response(const hipe_instruction& ev) {
	const char* path = ev.arg[0];
	DBG("FIFO_RESPONSE: path='%s' modes='%s' name='%s'\n",
		path ? path : "", ev.arg[1] ? ev.arg[1] : "", ev.arg[2] ? ev.arg[2] : "");
	if (!path || !path[0]) {
		/* Blank arg[0] == the user cancelled the picker, or the host rejected the request.
		 * Leave whatever is on screen (empty state, or the current document) as-is. */
		return;
	}

	const char* name_arg = ev.arg[2]; /* host-assigned display filename; may be empty */
	std::string display_name = (name_arg && name_arg[0]) ? name_arg : path;

	/* A real file (top level: Hipe handed back an openable path) vs a FIFO (framed: a named
	 * pipe to stream the bytes through). */
	struct stat st;
	bool is_fifo = (stat(path, &st) == 0 && S_ISFIFO(st.st_mode));

	try {
		if (is_fifo) {
			std::vector<uint8_t> data = read_fifo_resource(path);
			if (data.empty()) {
				fprintf(stderr, "open: no data received from FIFO '%s'\n", path);
				if (!doc) show_empty_state();
				return;
			}
			accept_document(std::make_unique<PdfDocument>(data, ".pdf"), display_name);
		} else {
			accept_document(std::make_unique<PdfDocument>(std::string(path)), display_name);
		}
	} catch (const std::exception& e) {
		fprintf(stderr, "open: failed to load '%s': %s\n", path, e.what());
		if (!doc) show_empty_state();
	}
}

/* ---- FIFO host role: another app pushes a document to us ("open with...") ---------------
 *
 * At startup (non-embedded only) advertise_fifo_ability() tells the framing manager we can
 * receive a PDF (HIPE_OP_FIFO_ADD_ABILITY). When a file shell then does "open with...", the
 * framing manager relays its HIPE_OP_FIFO_GET_PEER to us with the chosen ability name in
 * arg[3]; handle_incoming_fifo_get_peer() creates a pipe, answers with HIPE_OP_FIFO_RESPONSE,
 * opens the pipe for reading straight away (+ echoes FIFO_OPEN), drains the bytes the shell
 * writes, and loads them -- the same accept_document() path the Open button uses.
 *
 * Verified against periscope's relay (peer-list.cc / fiforequestmenu.cc): the file shell
 * sends FIFO_GET_PEER -> periscope's peer picker -> user chooses us -> periscope re-sends
 * FIFO_GET_PEER to us as {filename, accessModes, metadata, abilityName}, requestor = its
 * relationship handle, which it expects back verbatim on our FIFO_RESPONSE. Our ability is
 * only offered to a request whose minimal access mode contains "w" and whose metadata lists
 * "pdf" (Ability::isRequestCompatible).
 */

static void advertise_fifo_ability() {
	/* arg1 "w": the peer (the file shell) writes the document to us; we read it. arg2 is
	 * newline-separated: description line, then "ext:label" file-type patterns the framing
	 * manager matches senders against. */
	hipe_send(session, HIPE_OP_FIFO_ADD_ABILITY, 0, 0, 3,
		FIFO_HOST_ABILITY, "w",
		"Open a PDF document in the viewer\npdf:Portable Document Format");
	DBG("advertised FIFO ability \"%s\" (mode w, pdf)\n", FIFO_HOST_ABILITY);
}

static void handle_incoming_fifo_get_peer(const hipe_instruction& ev) {
	uint64_t requestor = ev.requestor; /* echo verbatim in our FIFO_RESPONSE */
	const char* suggested = ev.arg[0]; /* suggested name, no extension; may be blank */
	std::string display_name = (suggested && suggested[0]) ? suggested : "document";

	DBG("incoming FIFO_GET_PEER: requestor=%llu name='%s' modes='%s' ability='%s'\n",
		(unsigned long long) requestor, suggested ? suggested : "",
		ev.arg[1] ? ev.arg[1] : "", ev.arg[3] ? ev.arg[3] : "");

	/* Create the pipe we hand back. $XDG_RUNTIME_DIR is the right home for a transient
	 * per-user IPC object; fall back to /tmp. */
	const char* dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !dir[0]) dir = "/tmp";
	static unsigned seq = 0;
	char path[256];
	bool made = false;
	for (int attempt = 0; attempt < 1000; attempt++, seq++) {
		snprintf(path, sizeof(path), "%s/hipe-pdf-%ld-%u.pdf", dir, (long) getpid(), seq);
		if (mkfifo(path, 0600) == 0) { made = true; break; }
		if (errno != EEXIST) break;
	}
	if (!made) {
		fprintf(stderr, "fifo host: mkfifo in '%s': %s\n", dir, strerror(errno));
		hipe_send(session, HIPE_OP_FIFO_RESPONSE, requestor, 0, 1, ""); /* blank arg0 == reject */
		return;
	}
	host_fifo_path = path;
	DBG("created pipe %s; sending FIFO_RESPONSE\n", path);

	/* Grant "w" (a subset of what the client asked for): it writes, we read. */
	hipe_send(session, HIPE_OP_FIFO_RESPONSE, requestor, 0, 4,
		path, "w", display_name.c_str(), "pdf:Portable Document Format");

	/* Open our (read) end right away, before the client can act -- it only learns the path
	 * from the response we just sent, so we are certain to be reading before it writes.
	 * This means we do NOT block waiting for the client's FIFO_OPEN: drain_fifo()'s poll
	 * loop waits for the writer and is capped at 30s, so a file shell that never follows
	 * through leaves a bounded, visible failure rather than hanging the app. We still send
	 * our own FIFO_OPEN so a client that waits for the host echo can proceed; the client's
	 * own FIFO_OPEN, if it sends one, lands in the main loop and is ignored. */
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "fifo host: open('%s'): %s\n", path, strerror(errno));
		unlink(path);
		host_fifo_path.clear();
		return;
	}
	hipe_send(session, HIPE_OP_FIFO_OPEN, 0, 0, 2, path, "w"); /* our read end is open */
	DBG("read end open, echoed FIFO_OPEN; draining pipe (30s cap)\n");

	std::vector<uint8_t> bytes = drain_fifo(fd, path, "fifo host");
	close(fd);
	DBG("drained %zu bytes\n", bytes.size());

	hipe_send(session, HIPE_OP_FIFO_CLOSE, 0, 0, 1, path);
	hipe_send(session, HIPE_OP_FIFO_DROP_PEER, 0, 0, 1, path);
	unlink(path);
	host_fifo_path.clear();

	if (bytes.empty()) {
		fprintf(stderr, "fifo host: no data received for '%s'\n", display_name.c_str());
		if (!doc) show_empty_state();
		return;
	}
	try {
		accept_document(std::make_unique<PdfDocument>(bytes, ".pdf"), display_name);
	} catch (const std::exception& e) {
		fprintf(stderr, "fifo host: failed to load received document: %s\n", e.what());
		if (!doc) show_empty_state();
	}
}

int main(int argc, char** argv) {
	/* Two launch modes:
	 *   - with a file argument: "embedded document" mode -- open that file, no Open button
	 *     (like a PDF embedded in a web page).
	 *   - with no argument: start document-less, show the empty-state panel and an Open
	 *     button that pulls a file in through Hipe's FIFO framework (see
	 *     request_open_document / handle_fifo_response). The button stays available
	 *     afterwards for swapping documents in-session. */
	const char* startup_path = (argc >= 2) ? argv[1] : nullptr;
	bool embedded_mode = (startup_path != nullptr);

	/* Embedded mode fails fast, before opening a session, exactly as before -- a bad file
	 * passed explicitly is a launch error, not something to recover from with a picker. */
	std::unique_ptr<PdfDocument> startup_doc;
	if (startup_path) {
		try {
			startup_doc = std::make_unique<PdfDocument>(std::string(startup_path));
		} catch (const std::exception& e) {
			fprintf(stderr, "%s\n", e.what());
			return 2;
		}
		if (startup_doc->pageCount() <= 0) {
			fprintf(stderr, "No pages found in '%s'\n", startup_path);
			return 2;
		}
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
	/* Cursors are unicode glyphs set with SET_CURSOR, never CSS `cursor:` keywords (see the
	 * CURSOR_* defines). `cursor:inherit` on `*` stops <button> etc. from falling back to
	 * their native cursor, so setting SET_CURSOR on a container is enough for all its
	 * descendants (including ones appended later, e.g. link divs / text spans). The body
	 * default (arrow) covers almost everything; only #textLayer, #linkLayer and the zoom
	 * label override it below. render_and_show() swaps to CURSOR_BUSY around a slow render.
	 * Same approach as periscope's Screen. */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "*", "cursor:inherit;");
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, 0, 1, CURSOR_DEFAULT);
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
	/* pointer-events:auto here counteracts #textLayer's own pointer-events:none (see its
	 * DOM setup below) -- discovered this needed doing (not assumed) after rescoping the
	 * slideshow click-to-advance listener from main_area to img_page specifically: with
	 * an ancestor-scoped listener, event bubbling reached it regardless of which overlay
	 * absorbed the initial hit, but a sibling-scoped one doesn't, so #textLayer's own
	 * blank areas (identical structural pattern to the #linkLayer bug found earlier this
	 * session) turned out to silently swallow clicks meant for img_page underneath.
	 * pointer-events:none on the container fixes that; auto here keeps spans themselves
	 * hit-testable so drag-to-select still works (pointer-events is inherited, so without
	 * this override every span would inherit the container's none too). */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "#textLayer span",
		"position:absolute; color:transparent; white-space:nowrap; overflow:visible; "
		"pointer-events:auto; -webkit-user-select:text; user-select:text;");
	/* Shared link-overlay div properties; per-link geometry is set individually in
	 * update_link_layer(). user-select:none so a click-drag starting on a link doesn't
	 * fight with the text layer underneath for a selection instead of registering as a
	 * click. pointer-events:auto opts each individual link div back into hit-testing --
	 * see #linkLayer's own pointer-events:none below for why that's needed at all. */
	hipe_send(session, HIPE_OP_ADD_STYLE_RULE, 0, 0, 2, "#linkLayer div",
		"position:absolute; pointer-events:auto; "
		"-webkit-user-select:none; user-select:none;");

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
	/* The sidebar is populated by swap_in_document() once a document is loaded (which may
	 * be now, in embedded mode, or later via the Open button) -- not here, where there is
	 * no document yet. */

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, root, 2, "div", "main");
	main_area = get_by_id("main");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex", "1");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "flex-direction", "column");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "overflow", "hidden");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, main_area, 2, "position", "relative");
	/* inherit rather than a bare transparent -- same visual result when nothing else
	 * overrides it (body's color shows through around the page when zoomed out), and it's
	 * what leave_slideshow explicitly re-sets after slideshow's own opaque override, since
	 * no stylesheet rule targets #main_area for an empty value to fall back to. */
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

	/* The "no document open" panel, shown only when the app is launched with no file (see
	 * main()'s two launch modes). A child of #main_area (which is position:relative) rather
	 * than #viewport, filling it absolutely, so it sits over the empty page area regardless
	 * of #viewport's own scroll/centering state -- and below #navbar (z-index:10), so the
	 * Open button stays clickable on top of it. The whole panel is a click target for
	 * REQ_OPEN too, not just the toolbar button. Styled like #pageStatus (theme-inherited
	 * fill/text, dashed outline) for consistency with the loading/error overlay. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "emptyState");
	empty_state = get_by_id("emptyState");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "box-sizing", "border-box");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "display", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "align-items", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "justify-content", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "text-align", "center");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "white-space", "pre-line");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "background-color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "color", "inherit");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, empty_state, 2, "font-size", "15px");
	hipe_send(session, HIPE_OP_SET_TEXT, 0, empty_state, 1,
		"No document open\n\n\xf0\x9f\x93\x82  Click here (or Open) to choose a PDF");
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_OPEN, empty_state, 1, "click");

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
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, text_layer, 1, CURSOR_TEXT); /* spans inherit the I-beam */
	/* See the "#textLayer span" rule's comment above -- the container itself must not be
	 * hit-testable, only the individual spans (which opt back in via their own
	 * pointer-events:auto), or its blank areas swallow clicks meant for img_page below. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, text_layer, 2, "pointer-events", "none");

	/* Appended after (so painted above -- see #linkLayer's "div" rule above) text_layer:
	 * a link's clickable area should win a click over the selectable text underneath it
	 * (e.g. a URL rendered as visible page text that's also a live hyperlink), same
	 * reasoning as #pageStatus's explicit z-index over both -- given via z-index (not
	 * just DOM order) for the same "don't rely on it, be explicit" reason page_status
	 * already is, comfortably below page_status's own 2 so the loading/error overlay
	 * still wins over both when shown. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_wrapper, 2, "div", "linkLayer");
	link_layer = get_by_id("linkLayer");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "top", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "left", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "width", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "height", "100%");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "overflow", "hidden");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "z-index", "1");
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, link_layer, 1, CURSOR_POINTER); /* link divs inherit */
	/* Confirmed against hipecore's own source (RenderElement::visibleToHitTesting) that
	 * pointer-events genuinely gates hit-testing here, not just parses harmlessly like
	 * some other CSS this fork accepts but ignores -- essential, not decorative: without
	 * this, the container's own full-page box (blank everywhere except at actual link
	 * rects) sits topmost and swallows every click/drag-select on the ENTIRE page, not
	 * just within real links. Confirmed live: a text drag-select that worked before this
	 * layer existed silently selected nothing once it was added, until this fix. Each
	 * individual link div opts back in via its own pointer-events:auto (see the
	 * "#linkLayer div" rule above), so only their specific small rects intercept
	 * anything. */
	hipe_send(session, HIPE_OP_SET_STYLE, 0, link_layer, 2, "pointer-events", "none");

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
		"min-width:32px; padding:4px 8px; font-size:16px;");

	const char* GROUP_MARGIN = "22px";  /* between page nav / zoom / slideshow */
	const char* ITEM_MARGIN = "6px";    /* between controls within one group */

	/* "Open" button -- only in the no-file launch mode (see main()'s two modes). First
	 * child of #navbar, so it sits leftmost, ahead of the sidebar toggle. Not created at
	 * all in embedded mode, matching how an embedded PDF viewer has no way to load a
	 * different file. It's a child of #navbar so slideshow hides it automatically along
	 * with the rest of the toolbar. */
	if (!embedded_mode) {
		hipe_send(session, HIPE_OP_APPEND_TAG, 0, navbar, 2, "button", "openBtn");
		open_btn = get_by_id("openBtn");
		hipe_send(session, HIPE_OP_APPEND_TEXT, 0, open_btn, 1, "\xf0\x9f\x93\x82 Open" /* 📂 Open */);
		hipe_send(session, HIPE_OP_SET_STYLE, 0, open_btn, 2, "margin-right", GROUP_MARGIN);
		hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_OPEN, open_btn, 1, "click");
	}

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
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, prev_btn, 1, "\xe2\x8f\xb4" /* U+23F4 ⏴, matches the slideshow dialog's own prev symbol */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, prev_btn, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_group, 2, "span", "pageLabel");
	page_label = get_by_id("pageLabel");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, page_label, 2, "margin-right", ITEM_MARGIN);

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, page_group, 2, "button", "nextBtn");
	hipe_loc next_btn = get_by_id("nextBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, next_btn, 1, "\xe2\x8f\xb5" /* U+23F5 ⏵, matches the slideshow dialog's own next symbol */);

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
	/* Clicking the label itself resets to 100% zoom -- CURSOR_POINTER is the only affordance
	 * for this (no tooltip support to spell it out). It and hyperlinks are the only things
	 * that get the hand; buttons/thumbnails keep the default arrow. */
	hipe_send(session, HIPE_OP_SET_CURSOR, 0, zoom_label, 1, CURSOR_POINTER);

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

	/* Overlay button pair, only shown once slideshow mode hides the sidebar/navbar -- a
	 * guaranteed way to open the menu or exit that doesn't depend on right-click dialog
	 * support (which varies by framing manager, see HIPE_OP_DIALOG notes in CLAUDE.md) or
	 * on having a mouse to right-click with at all (a touchscreen presenter has no
	 * contextmenu gesture, hence the separate Menu button -- see show_slideshow_dialog).
	 * Wrapped in one flex container so both fade in/out together (see
	 * reveal_slideshow_controls/hide_slideshow_controls) rather than needing to toggle
	 * each button's own display individually; margin-right on the first button stands in
	 * for a flex "gap" (not supported by this WebKit fork, confirmed earlier). Bottom-left,
	 * old-PowerPoint-presenter-view style, rather than top-right. display stays flex
	 * permanently -- display:none/block can't be CSS-transitioned, and reveal/hide fades
	 * via opacity+pointer-events instead (see those functions) so the pair genuinely fades
	 * rather than snapping in/out. */
	hipe_send(session, HIPE_OP_APPEND_TAG, 0, main_area, 2, "div", "slideshowControls");
	slideshow_controls = get_by_id("slideshowControls");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "display", "flex");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "opacity", "0");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "pointer-events", "none");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "transition", "opacity 0.4s ease");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "position", "absolute");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "bottom", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "left", "10px");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_controls, 2, "align-items", "center");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, slideshow_controls, 2, "button", "slideshowMenuBtn");
	slideshow_menu_btn = get_by_id("slideshowMenuBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, slideshow_menu_btn, 1, "\xe2\x8f\xb6" /* U+23F6 ⏶, the classic PowerPoint presenter-menu glyph -- compact icon-only, no label */);
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_menu_btn, 2, "opacity", "0.6");
	hipe_send(session, HIPE_OP_SET_STYLE, 0, slideshow_menu_btn, 2, "margin-right", "8px");

	hipe_send(session, HIPE_OP_APPEND_TAG, 0, slideshow_controls, 2, "button", "slideshowLeaveBtn");
	slideshow_leave_btn = get_by_id("slideshowLeaveBtn");
	hipe_send(session, HIPE_OP_APPEND_TEXT, 0, slideshow_leave_btn, 1, "\xe2\x9c\x95 Leave Slideshow");
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
	/* Shares REQ_SLIDESHOW_MENU with contextmenu's own registration in enter_slideshow --
	 * dispatch only switches on the requestor value, so both routes to show_slideshow_dialog()
	 * without needing a second case in the event loop. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_SLIDESHOW_MENU, slideshow_menu_btn, 1, "click");
	/* REQ_SLIDESHOW_ADVANCE's "click" on img_page is requested/cancelled in
	 * enter_slideshow()/leave_slideshow() instead of unconditionally here -- see the
	 * comment there (matches "contextmenu"'s existing pattern: only meaningful while
	 * actually in slideshow). */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_KEYDOWN, 0, 1, "keydown"); /* location 0 = whole-frame keydown */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_WHEEL, viewport, 1, "wheel");
	/* "resize" is special-cased server-side to attach to the window regardless of the
	 * location given (see requestEvent() in hipecore's qwebelement.cpp), so location is
	 * arbitrary here -- 0 to match the other whole-frame requests above. */
	hipe_send(session, HIPE_OP_EVENT_REQUEST, REQ_RESIZE, 0, 1, "resize");

	if (startup_doc) {
		/* Embedded mode: show the file that was passed on the command line.
		 * swap_in_document() does the initial content-area sizing + first render itself. */
		swap_in_document(std::move(startup_doc), startup_path);
	} else {
		/* No-file mode: wait for the user to pick something via the Open button, or for
		 * another app to push a document to us. */
		show_empty_state();
		advertise_fifo_ability();
	}

	hipe_instruction event;
	hipe_instruction_init(&event);
	do {
		/* Only while slideshow_active does this loop need to wake up on its own (to
		 * notice the Menu/Leave pair's idle-fade timeout has elapsed with no new event
		 * having arrived to trigger a check) -- outside slideshow there's nothing to poll
		 * for, so the normal indefinite blocking wait is used, exactly as before, to avoid
		 * spending any CPU when nothing is happening (per this project's "lean/efficient"
		 * goal). blocking=0 returns immediately with 0 if nothing is queued. */
		short got = hipe_next_instruction(session, &event, slideshow_active ? 0 : 1);
		/* Per the Hipe API docs: always check for a -1 return (disconnection) or an
		 * HIPE_OP_SERVER_DENIED opcode and exit -- otherwise an orphaned client (e.g.
		 * hiped restarting/crashing out from under it) spins forever, since a blocking
		 * call can no longer actually block on anything once disconnected. Only check
		 * event.opcode when got > 0 -- with got == 0 (nothing queued, only possible in
		 * the non-blocking/slideshow case above) event was never written this iteration
		 * and still holds whatever the previous iteration left in it. */
		if (got < 0 || (got > 0 && event.opcode == HIPE_OP_SERVER_DENIED))
			break;
		if (got == 0) {
			check_slideshow_controls_idle_timeout();
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			continue;
		}

		if (event.opcode == HIPE_OP_DIALOG_RETURN) {
			if (event.requestor == REQ_SLIDESHOW_DIALOG) handle_slideshow_dialog_return(event);
			continue;
		}

		/* The environment's answer to a request_open_document() -- a real file path (top
		 * level) or a FIFO path (framed). Handled here, like DIALOG_RETURN above, rather
		 * than in the HIPE_OP_EVENT dispatch below. */
		if (event.opcode == HIPE_OP_FIFO_RESPONSE) {
			if (event.requestor == REQ_FIFO_GET_PDF) handle_fifo_response(event);
			if (g_should_exit) break;
			continue;
		}

		/* Another app asking to hand us a document ("open with...") -- the FIFO host role,
		 * matching the "Open" ability we advertised at startup. Ignored in embedded mode,
		 * where it was never advertised. */
		if (event.opcode == HIPE_OP_FIFO_GET_PEER) {
			if (!embedded_mode) handle_incoming_fifo_get_peer(event);
			if (g_should_exit) break;
			continue;
		}

		/* Leftover handshake traffic that lands after a synchronous FIFO exchange (host or
		 * client) already ran to completion -- the client's own FIFO_OPEN/CLOSE, or a
		 * DROP_PEER. handle_incoming_fifo_get_peer() drains and unlinks its pipe itself, so
		 * these are normally no-ops; DROP_PEER is honoured as a backstop for a pipe still on
		 * disk (e.g. our handler returned early). Never unlink on FIFO_OPEN -- that can
		 * arrive mid-transfer. */
		if (event.opcode == HIPE_OP_FIFO_OPEN || event.opcode == HIPE_OP_FIFO_CLOSE) {
			DBG("ignoring stray %s\n", event.opcode == HIPE_OP_FIFO_OPEN ? "FIFO_OPEN" : "FIFO_CLOSE");
			continue;
		}
		if (event.opcode == HIPE_OP_FIFO_DROP_PEER) {
			if (!host_fifo_path.empty() && event.arg[0] && host_fifo_path == event.arg[0]) {
				unlink(host_fifo_path.c_str());
				host_fifo_path.clear();
			}
			continue;
		}

		if (event.opcode != HIPE_OP_EVENT) {
			if (event.opcode != HIPE_OP_FRAME_CLOSE)
				DBG("unhandled instruction opcode=%d\n", (int) event.opcode);
			continue;
		}
		/* requestor is only meaningful on HIPE_OP_EVENT replies to our own
		 * EVENT_REQUESTs -- other instruction types can carry unrelated
		 * requestor values that happen to collide with our REQ_* codes. */
		if (event.requestor == REQ_OPEN) request_open_document();
		/* Every other action needs a loaded document. With none (the no-file launch mode,
		 * before the first Open), the toolbar isn't shown and #emptyState only emits
		 * REQ_OPEN -- but guard anyway so a stray event can't reach code that dereferences
		 * doc. */
		else if (!doc) { /* nothing */ }
		else if (event.requestor == REQ_SIDEBAR_TOGGLE) toggle_sidebar();
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
		else if (event.requestor == REQ_SLIDESHOW_MOUSEMOVE && slideshow_active) reveal_slideshow_controls();
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
		/* Checked before the REQ_THUMB_BASE range below despite both being open-ended
		 * ">=" comparisons -- REQ_LINK_BASE sits far above REQ_THUMB_BASE's own range
		 * specifically so a link click can never be misread as an out-of-range thumbnail
		 * click, but only if this check runs first. */
		else if (event.requestor >= REQ_LINK_BASE) {
			int idx = (int) (event.requestor - REQ_LINK_BASE);
			if (idx >= 0 && idx < (int) current_page_links.size()) {
				const auto& link = current_page_links[idx];
				if (link.is_external) hipe_send(session, HIPE_OP_OPEN_LINK, 0, 0, 1, link.uri.c_str());
				else render_and_show(link.target_page);
			}
		}
		else if (event.requestor >= REQ_THUMB_BASE) render_and_show((int) (event.requestor - REQ_THUMB_BASE));
	} while (event.opcode != HIPE_OP_FRAME_CLOSE);

	if (!embedded_mode)
		hipe_send(session, HIPE_OP_FIFO_REMOVE_ABILITY, 0, 0, 1, FIFO_HOST_ABILITY);
	if (!host_fifo_path.empty())
		unlink(host_fifo_path.c_str());

	hipe_close_session(session);
	return 0;
}
