#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct fz_context;
struct fz_document;

class PdfDocument {
	/* RAII wrapper around a MuPDF fz_context + fz_document pair. */
public:
	explicit PdfDocument(const std::string& path);
	/* Throws std::runtime_error if the file can't be opened or parsed. */

	PdfDocument(const std::vector<uint8_t>& bytes, const std::string& magic);
	/* Opens a document from an in-memory copy of its bytes rather than a path on disk --
	 * used when the file arrives streamed through a Hipe FIFO resource (see
	 * read_fifo_resource in main.cpp) instead of as an openable filesystem path. magic is
	 * a filename or extension (e.g. ".pdf") used only for format detection. Throws
	 * std::runtime_error if the bytes can't be parsed. */

	~PdfDocument();

	PdfDocument(const PdfDocument&) = delete;
	PdfDocument& operator=(const PdfDocument&) = delete;

	int pageCount() const;

	void pageSize(int pageNumber, float* widthPts, float* heightPts) const;
	/* Native page size in PDF points (1/72 inch), without rasterizing it. Throws
	 * std::runtime_error on failure. */

	static constexpr int DEFAULT_RENDER_TIMEOUT_MS = 10000;

	std::vector<uint8_t> renderPagePng(int pageNumber, float targetWidthPx,
		int timeoutMs = DEFAULT_RENDER_TIMEOUT_MS) const;
	/* Rasterizes the given 0-indexed page to a PNG image scaled so its width matches
	 * targetWidthPx (height follows the page's aspect ratio). Throws std::runtime_error
	 * on failure, including if the render takes longer than timeoutMs (a pathologically
	 * complex page is aborted rather than left to hang indefinitely) -- callers willing to
	 * accept a lower-resolution fallback can retry at a smaller targetWidthPx and/or a
	 * shorter timeoutMs, since a much smaller raster from the same pathological page is
	 * often (not guaranteed) tractable well within a fraction of the original budget. */

	void pageBackgroundColor(int pageNumber, uint8_t* r, uint8_t* g, uint8_t* b) const;
	/* Cheap heuristic for chrome-matching purposes (e.g. a slideshow surround): renders
	 * the page very small and averages its four corner pixels, on the assumption that
	 * page corners are usually background rather than content. Not real background
	 * detection. Falls back to white on failure rather than throwing. */

	struct TextSpan {
		std::string text;
		float x, y, width, height; /* PDF points, same page space renderPagePng scales from */
	};

	std::vector<TextSpan> pageTextSpans(int pageNumber) const;
	/* One span per line of extracted text, for building a selectable text overlay atop
	 * the raster render (positions/sizes are in PDF points -- the caller scales them by
	 * the same factor used for the raster width). Throws std::runtime_error on failure. */

	struct PageLink {
		float x, y, width, height; /* PDF points, same page space as TextSpan */
		bool is_external; /* true: uri is a real URL (open via the display server/
			framing manager); false: an internal link -- target_page is what to jump to */
		std::string uri;
		int target_page; /* 0-indexed, flat across chapters (see fz_page_number_from_location);
			-1 if internal but unresolvable. Meaningless when is_external. */
	};

	std::vector<PageLink> pageLinks(int pageNumber) const;
	/* One entry per clickable link region on the page (hyperlinks and internal page-jump
	 * links), for building a clickable overlay atop the raster render the same way
	 * pageTextSpans builds the selectable text overlay -- positions/sizes are in PDF
	 * points, scaled by the caller the same way. Throws std::runtime_error on failure. */

private:
	void initContext();
	/* Shared first half of both constructors: creates the fz_context and registers the
	 * document handlers. Throws std::runtime_error if the context can't be created. */

	fz_context* ctx;
	fz_document* doc;
};
