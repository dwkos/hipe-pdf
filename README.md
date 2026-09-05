PDF viewer utility for Hipe display server.

This is a minimal PDF viewer built in C++. It should use the hipe API as its main dependency
and avoid dynamic dependencies for maximum portability.

Wishlist for features:
- A sidebar to allow page thumbnails to be navigated. Page thumbnails should be rendered as lean as possible; efficiency over aesthetics to a certain point.

- zooming of current page

- A slideshow button to allow current page to occupy full frame, click to advance to next page, context-click to pop up dialog allowing next page, prev page, start, end, leave slideshow.

- Use SVG rendering. Hipe may or may not yet support DOM SVGs vs existing support for <img>s with svg byte data at time of implementation.

- eventual continuous scrolling allowing one page to appear to follow the next linearly, but not actually rendering pages outside the current scroll view.



