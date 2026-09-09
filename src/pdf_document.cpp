#include "pdf_document.hpp"

#include <mupdf/fitz.h>

#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

void PdfDocument::initContext() {
	ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!ctx)
		throw std::runtime_error("could not create MuPDF context");

	fz_register_document_handlers(ctx);
}

PdfDocument::PdfDocument(const std::string& path) : ctx(nullptr), doc(nullptr) {
	initContext();

	fz_try(ctx) {
		doc = fz_open_document(ctx, path.c_str());
	} fz_catch(ctx) {
		std::string message = fz_caught_message(ctx);
		fz_drop_context(ctx);
		throw std::runtime_error("failed to open '" + path + "': " + message);
	}
}

PdfDocument::PdfDocument(const std::vector<uint8_t>& bytes, const std::string& magic)
	: ctx(nullptr), doc(nullptr) {
	initContext();

	/* fz_open_buffer (reached via fz_open_document_with_buffer) takes its own reference to
	 * the buffer and the resulting document keeps the stream wrapping it alive, so our own
	 * reference can be dropped as soon as the document is open -- the bytes stay valid for
	 * the document's lifetime regardless. fz_new_buffer_from_copied_data copies, so the
	 * caller's vector doesn't need to outlive this call. */
	fz_buffer* buf = nullptr;
	fz_var(buf);

	fz_try(ctx) {
		buf = fz_new_buffer_from_copied_data(ctx, bytes.data(), bytes.size());
		doc = fz_open_document_with_buffer(ctx, magic.c_str(), buf);
	} fz_always(ctx) {
		fz_drop_buffer(ctx, buf);
	} fz_catch(ctx) {
		std::string message = fz_caught_message(ctx);
		fz_drop_context(ctx);
		throw std::runtime_error("failed to open in-memory document: " + message);
	}
}

PdfDocument::~PdfDocument() {
	if (doc) fz_drop_document(ctx, doc);
	if (ctx) fz_drop_context(ctx);
}

int PdfDocument::pageCount() const {
	int count = 0;
	fz_try(ctx) {
		count = fz_count_pages(ctx, doc);
	} fz_catch(ctx) {
		throw std::runtime_error(std::string("pageCount: ") + fz_caught_message(ctx));
	}
	return count;
}

void PdfDocument::pageSize(int pageNumber, float* widthPts, float* heightPts) const {
	fz_page* page = nullptr;
	fz_var(page);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);
		fz_rect bounds = fz_bound_page(ctx, page);
		if (widthPts) *widthPts = bounds.x1 - bounds.x0;
		if (heightPts) *heightPts = bounds.y1 - bounds.y0;
	} fz_always(ctx) {
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		throw std::runtime_error("pageSize: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}
}

std::vector<uint8_t> PdfDocument::renderPagePng(int pageNumber, float targetWidthPx, int timeoutMs) const {
	std::vector<uint8_t> result;

	fz_page* page = nullptr;
	fz_device* dev = nullptr;
	fz_pixmap* pix = nullptr;
	fz_buffer* buf = nullptr;
	fz_output* out = nullptr;

	fz_var(page);
	fz_var(dev);
	fz_var(pix);
	fz_var(buf);
	fz_var(out);

	fz_cookie cookie = {0, 0, (size_t) -1, 0, 0};
	bool timed_out = false;

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);

		fz_rect bounds = fz_bound_page(ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float scale = (pageWidth > 0) ? (targetWidthPx / pageWidth) : 1.0f;
		fz_matrix ctm = fz_scale(scale, scale);

		fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
		pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, 0);
		fz_clear_pixmap_with_value(ctx, pix, 0xFF);

		dev = fz_new_draw_device(ctx, ctm, pix);

		/* Watchdog: only touches cookie.abort (never another fz_* call), so it's safe to run
		 * concurrently with fz_run_page even though this fz_context has no locking support.
		 * Waits on a condition variable rather than a flat sleep so a fast render isn't
		 * penalized with added latency waiting for the watchdog to notice completion. */
		std::mutex m;
		std::condition_variable cv;
		bool done = false;
		std::thread watchdog([&]() {
			std::unique_lock<std::mutex> lock(m);
			if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return done; })) {
				cookie.abort = 1;
			}
		});

		/* fz_identity, not ctm -- the transform is already baked into the device (see
		 * fz_new_draw_device above); passing it again here would double-apply the scale
		 * (matches the pattern in MuPDF's own fz_new_pixmap_from_page_with_separations). */
		fz_run_page(ctx, page, dev, fz_identity, &cookie);

		{
			std::lock_guard<std::mutex> lock(m);
			done = true;
		}
		cv.notify_one();
		watchdog.join();

		fz_close_device(ctx, dev);

		if (cookie.abort) {
			timed_out = true;
		} else {
			buf = fz_new_buffer(ctx, 4096);
			out = fz_new_output_with_buffer(ctx, buf);
			fz_write_pixmap_as_png(ctx, out, pix);
			fz_close_output(ctx, out);

			unsigned char* data;
			size_t size = fz_buffer_storage(ctx, buf, &data);
			result.assign(data, data + size);
		}
	} fz_always(ctx) {
		if (out) fz_drop_output(ctx, out);
		if (buf) fz_drop_buffer(ctx, buf);
		if (dev) fz_drop_device(ctx, dev);
		if (pix) fz_drop_pixmap(ctx, pix);
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		throw std::runtime_error("renderPagePng: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}

	if (timed_out) {
		throw std::runtime_error("renderPagePng: page " + std::to_string(pageNumber) +
			": timed out after " + std::to_string(timeoutMs) + "ms (page too complex to render)");
	}

	return result;
}

void PdfDocument::pageBackgroundColor(int pageNumber, uint8_t* r, uint8_t* g, uint8_t* b) const {
	const float SAMPLE_WIDTH_PX = 32.0f;

	int sumR = 255, sumG = 255, sumB = 255; /* fall back to white */

	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	fz_var(page);
	fz_var(pix);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);
		fz_rect bounds = fz_bound_page(ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float scale = (pageWidth > 0) ? (SAMPLE_WIDTH_PX / pageWidth) : 1.0f;
		pix = fz_new_pixmap_from_page(ctx, page, fz_scale(scale, scale), fz_device_rgb(ctx), 0);

		int w = fz_pixmap_width(ctx, pix);
		int h = fz_pixmap_height(ctx, pix);
		int n = fz_pixmap_components(ctx, pix);
		ptrdiff_t stride = fz_pixmap_stride(ctx, pix);
		unsigned char* samples = fz_pixmap_samples(ctx, pix);

		if (w > 0 && h > 0 && n >= 3) {
			int xs[2] = {0, w - 1};
			int ys[2] = {0, h - 1};
			sumR = sumG = sumB = 0;
			for (int yi = 0; yi < 2; yi++) {
				for (int xi = 0; xi < 2; xi++) {
					unsigned char* p = samples + ys[yi] * stride + xs[xi] * n;
					sumR += p[0]; sumG += p[1]; sumB += p[2];
				}
			}
			sumR /= 4; sumG /= 4; sumB /= 4;
		}
	} fz_always(ctx) {
		if (pix) fz_drop_pixmap(ctx, pix);
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		/* keep the white fallback set above */
	}

	if (r) *r = (uint8_t) sumR;
	if (g) *g = (uint8_t) sumG;
	if (b) *b = (uint8_t) sumB;
}

std::vector<PdfDocument::TextSpan> PdfDocument::pageTextSpans(int pageNumber) const {
	std::vector<TextSpan> spans;

	fz_stext_page* stext = nullptr;
	fz_var(stext);

	fz_try(ctx) {
		fz_stext_options opts;
		fz_init_stext_options(ctx, &opts);
		stext = fz_new_stext_page_from_page_number(ctx, doc, pageNumber, &opts);

		for (fz_stext_block* block = stext->first_block; block; block = block->next) {
			if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
			for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
				/* Skip rotated/angled lines (e.g. street names following a diagonal
				 * road on a map) -- line->bbox is axis-aligned, so for a tilted line
				 * its "height" is actually dominated by the text's rotated reading
				 * length, not its font size. The caller (see update_text_layer in
				 * main.cpp) derives font-size directly from bbox height, so a
				 * skewed line there produces a wildly oversized span -- invisible
				 * normally (the overlay is fully transparent) but revealed as a
				 * giant selection-highlighted mess the moment it's selected, which
				 * also visibly stalls this WebKit fork given enough such spans to
				 * hit-test. dir is the line's normalized baseline direction ((1,0)
				 * for plain horizontal text); a generous tilt tolerance still allows
				 * slightly-skewed-but-basically-horizontal text through. */
				if (fabsf(line->dir.y) > 0.2f) continue;

				std::string text;
				for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
					char utf8[4];
					int len = fz_runetochar(utf8, ch->c);
					text.append(utf8, len);
				}
				if (text.empty()) continue;

				TextSpan span;
				span.text = text;
				span.x = line->bbox.x0;
				span.y = line->bbox.y0;
				span.width = line->bbox.x1 - line->bbox.x0;
				span.height = line->bbox.y1 - line->bbox.y0;
				spans.push_back(std::move(span));
			}
		}
	} fz_always(ctx) {
		if (stext) fz_drop_stext_page(ctx, stext);
	} fz_catch(ctx) {
		throw std::runtime_error("pageTextSpans: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}

	return spans;
}

std::vector<PdfDocument::PageLink> PdfDocument::pageLinks(int pageNumber) const {
	std::vector<PageLink> result;

	fz_page* page = nullptr;
	fz_link* links = nullptr;
	fz_var(page);
	fz_var(links);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);
		links = fz_load_links(ctx, page);

		for (fz_link* link = links; link; link = link->next) {
			if (!link->uri || !link->uri[0]) continue; /* no destination -- nothing to do on click */

			PageLink pl;
			pl.x = link->rect.x0;
			pl.y = link->rect.y0;
			pl.width = link->rect.x1 - link->rect.x0;
			pl.height = link->rect.y1 - link->rect.y0;
			pl.is_external = fz_is_external_link(ctx, link->uri);
			pl.target_page = -1;

			if (pl.is_external) {
				pl.uri = link->uri;
			} else {
				fz_location loc = fz_resolve_link(ctx, doc, link->uri, nullptr, nullptr);
				if (loc.chapter >= 0)
					pl.target_page = fz_page_number_from_location(ctx, doc, loc);
			}

			result.push_back(std::move(pl));
		}
	} fz_always(ctx) {
		if (links) fz_drop_link(ctx, links);
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		throw std::runtime_error("pageLinks: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}

	return result;
}
