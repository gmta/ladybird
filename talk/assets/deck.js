(function () {
    "use strict";

    const slides = Array.from(document.querySelectorAll(".slide"));
    const deck = document.querySelector(".deck");
    const counter = document.querySelector("[data-slide-counter]");
    const progress = document.querySelector(".progress");
    const title = document.querySelector("title");
    const sourceZoomRules = document.createElement("style");
    document.head.append(sourceZoomRules);
    let index = 0;
    let introPlaybackStarted = false;

    function clamp(value, min, max) {
        return Math.max(min, Math.min(max, value));
    }

    function parseHash() {
        const match = window.location.hash.match(/^#\/(\d+)$/);
        if (!match) return 0;
        return clamp(Number(match[1]) - 1, 0, slides.length - 1);
    }

    function updateChrome(updateHash) {
        if (counter) counter.textContent = `${index + 1} / ${slides.length}`;
        if (progress) progress.style.setProperty("--progress", `${((index + 1) / slides.length) * 100}%`);
        if (title) title.textContent = `${index + 1}. ${slides[index].dataset.title || "CSS, From Text to Pixels"}`;
        if (updateHash) window.history.replaceState(null, "", `#/${index + 1}`);
    }

    function updateSourceZoomGeometry() {
        const viewportWidth = window.innerWidth || document.documentElement.clientWidth;
        const viewportHeight = window.innerHeight || document.documentElement.clientHeight;
        const slideMargin = viewportWidth * 0.07;
        const targetWidth = viewportWidth - slideMargin * 2;
        const targetMaxHeight = viewportHeight * 0.86;
        let rules = "";

        document.querySelectorAll(".source-zoom-shell").forEach(shell => {
            const snippet = shell.firstElementChild;
            if (snippet) shell.before(snippet);
            shell.remove();
        });
        document.querySelectorAll(".source-zoom-ready").forEach(snippet => {
            snippet.classList.remove("source-zoom-ready");
            snippet.removeAttribute("data-source-zoom-id");
            snippet.removeAttribute("tabindex");
        });
        sourceZoomRules.textContent = "";

        const activeSlide = slides[index];
        const snippets = [];
        if (activeSlide.classList.contains("code-slide"))
            activeSlide
                .querySelectorAll(".code-colored:not(.no-source-zoom)")
                .forEach(snippet => snippets.push(snippet));
        if (activeSlide.classList.contains("zoom-slide"))
            activeSlide.querySelectorAll(".zoomable-media").forEach(snippet => snippets.push(snippet));

        if (!snippets.length) {
            sourceZoomRules.textContent = "";
            return;
        }

        activeSlide.offsetWidth;
        const slideRect = activeSlide.getBoundingClientRect();
        const measurements = snippets.map((snippet, snippetIndex) => {
            const rect = snippet.getBoundingClientRect();
            return {
                computedStyle: window.getComputedStyle(snippet),
                initialLeft: rect.left - slideRect.left,
                initialTop: rect.top - slideRect.top,
                isZoomableMedia: snippet.classList.contains("zoomable-media"),
                rect,
                snippet,
                zoomID: `source-zoom-${index}-${snippetIndex}`,
            };
        });

        measurements.forEach(({ computedStyle, initialLeft, initialTop, isZoomableMedia, rect, snippet, zoomID }) => {
            const shell = document.createElement("div");
            shell.className = "source-zoom-shell";
            shell.style.width = `${rect.width}px`;
            shell.style.height = `${rect.height}px`;
            shell.style.marginTop = computedStyle.marginTop;
            shell.style.marginRight = computedStyle.marginRight;
            shell.style.marginBottom = computedStyle.marginBottom;
            shell.style.marginLeft = computedStyle.marginLeft;
            snippet.before(shell);
            shell.append(snippet);

            const targetTop = isZoomableMedia ? viewportHeight * 0.07 : initialTop;
            snippet.setAttribute("tabindex", "0");
            snippet.setAttribute("data-source-zoom-id", zoomID);
            snippet.classList.add("source-zoom-ready");
            const baseHeight = isZoomableMedia ? `height: ${rect.height}px; ` : "";
            const targetHeight = isZoomableMedia ? `height: ${targetMaxHeight}px; ` : "";
            rules += `.slide.active [data-source-zoom-id="${zoomID}"] { position: absolute; left: ${initialLeft}px; top: ${initialTop}px; width: ${rect.width}px; margin: 0; ${baseHeight}max-width: ${rect.width}px; max-height: ${rect.height}px; }\n`;
            rules += `.slide.active [data-source-zoom-id="${zoomID}"]:hover, .slide.active [data-source-zoom-id="${zoomID}"]:focus, .slide.active [data-source-zoom-id="${zoomID}"]:focus-within { left: ${slideMargin}px; top: ${targetTop}px; width: ${targetWidth}px; ${targetHeight}max-width: ${targetWidth}px; max-height: ${targetMaxHeight}px; }\n`;
        });
        sourceZoomRules.textContent = rules;
    }

    function showSlide(nextIndex, updateHash) {
        const previousIndex = index;
        index = clamp(nextIndex, 0, slides.length - 1);
        const useCrossFade =
            slides[previousIndex] &&
            slides[previousIndex].classList.contains("intro-slide") &&
            index === previousIndex + 1;
        slides.forEach(slide => slide.classList.remove("is-entering"));
        if (deck) {
            if (useCrossFade) deck.classList.add("cross-fade");
            else deck.classList.remove("cross-fade");
        }
        slides.forEach((slide, slideIndex) => {
            const active = slideIndex === index;
            slide.classList.toggle("active", active);
            slide.setAttribute("aria-hidden", active ? "false" : "true");
            slide.querySelectorAll("video, audio").forEach(media => {
                if (!active && !media.paused) media.pause();
            });
        });

        // Force a fresh entry animation even when rapidly moving back and forth.
        slides[index].offsetWidth;
        slides[index].classList.add("is-entering");

        if (slides[index] && slides[index].querySelector("[data-intro-video]")) resetIntroVideo();

        updateChrome(updateHash);
        updateSourceZoomGeometry();

        if (deck && useCrossFade) {
            setTimeout(() => {
                slides.forEach(slide => slide.classList.remove("is-entering"));
                deck.classList.remove("cross-fade");
            }, 760);
        }
    }

    function activeIntroVideo() {
        const activeSlide = slides[index];
        if (!activeSlide) return null;
        return activeSlide.querySelector("[data-intro-video]");
    }

    function resetIntroVideo() {
        const video = activeIntroVideo();
        introPlaybackStarted = false;
        if (!video) return;
        video.pause();
        video.currentTime = 0;
    }

    function startIntroVideoIfNeeded() {
        const video = activeIntroVideo();
        if (!video || introPlaybackStarted) return false;
        introPlaybackStarted = true;
        video.currentTime = 0;
        video.play();
        return true;
    }

    function next() {
        if (index >= slides.length - 1) return;
        const video = activeIntroVideo();
        if (video && introPlaybackStarted) {
            video.pause();
            showSlide(index + 1, true);
            return;
        }
        if (startIntroVideoIfNeeded()) return;
        showSlide(index + 1, true);
    }

    function previous() {
        if (index <= 0) return;
        showSlide(index - 1, true);
    }

    document.addEventListener("keydown", event => {
        if (event.target && ["INPUT", "SELECT", "TEXTAREA", "BUTTON"].includes(event.target.tagName)) return;
        if (["ArrowRight", "PageDown", " ", "Enter"].includes(event.key)) {
            event.preventDefault();
            next();
        } else if (["ArrowLeft", "PageUp", "Backspace"].includes(event.key)) {
            event.preventDefault();
            previous();
        } else if (event.key === "Home") {
            event.preventDefault();
            showSlide(0, true);
        } else if (event.key === "End") {
            event.preventDefault();
            showSlide(slides.length - 1, true);
        }
    });

    slides.forEach(slide => {
        slide.addEventListener("click", event => {
            if (event.target && ["INPUT", "SELECT", "TEXTAREA", "BUTTON"].includes(event.target.tagName)) return;
            if (slide !== slides[index]) return;
            if (activeIntroVideo()) {
                next();
                event.preventDefault();
            }
        });
    });
    window.addEventListener("hashchange", () => showSlide(parseHash(), false));

    function setupGrammarDemo() {
        const property = document.querySelector("[data-grammar-property]");
        const value = document.querySelector("[data-grammar-value]");
        const output = document.querySelector("[data-grammar-output]");
        if (!property || !value || !output) return;

        const probe = document.createElement("div");

        function escapeHTML(text) {
            return text.replace(
                /[&<>"']/g,
                character =>
                    ({
                        "&": "&amp;",
                        "<": "&lt;",
                        ">": "&gt;",
                        '"': "&quot;",
                        "'": "&#39;",
                    })[character]
            );
        }

        function declarationAccepted(prop, text) {
            if (!text) return false;

            if (window.CSS && typeof CSS.supports === "function") {
                try {
                    if (CSS.supports(prop, text)) return true;
                } catch (error) {
                    // Fall through to CSSOM parsing below.
                }
            }

            probe.style.cssText = "";
            probe.style.setProperty(prop, text);
            return probe.style.getPropertyValue(prop) !== "";
        }

        function update() {
            const prop = property.value;
            const text = value.value.trim();
            const accepted = declarationAccepted(prop, text);
            const safeProp = escapeHTML(prop);
            const safeText = escapeHTML(text);
            output.innerHTML = accepted
                ? `<span class="ok">accepted</span>: ${safeProp}: ${safeText}`
                : `<span class="bad">rejected</span>: ${safeProp}: ${safeText}`;
        }

        property.addEventListener("change", update);
        property.addEventListener("input", update);
        value.addEventListener("input", update);
        value.addEventListener("change", update);
        update();
    }

    function setupCascadeDemo() {
        const specificity = document.querySelector("[data-cascade-specificity]");
        const specificityValue = document.querySelector("[data-cascade-specificity-value]");
        const sourceOrder = document.querySelector("[data-cascade-source-order]");
        const sourceOrderValue = document.querySelector("[data-cascade-source-order-value]");
        const important = document.querySelector("[data-cascade-important]");
        const output = document.querySelector("[data-cascade-output]");
        const target = document.querySelector("[data-cascade-target]");
        if (!specificity || !sourceOrder || !important || !output) return;

        function compareDeclarations(first, second) {
            if (first.important !== second.important) return first.important - second.important;
            if (first.specificity !== second.specificity) return first.specificity - second.specificity;
            return first.sourceOrder - second.sourceOrder;
        }

        const candidateSelectors = [
            { selector: "button", specificity: 1 },
            { selector: ".action", specificity: 10 },
            { selector: "button.action", specificity: 11 },
            { selector: "main button.action", specificity: 12 },
        ];

        function cssRule(declaration, winner) {
            return `<pre class="cascade-rule${winner ? " winner" : ""}"><code>${declaration.selector} {\n    color: ${declaration.value}${declaration.important ? " !important" : ""};\n}</code></pre>`;
        }

        function update() {
            const candidateSelector = candidateSelectors[Number(specificity.value)];
            const candidateComesLater = sourceOrder.checked;
            const author = {
                selector: candidateSelector.selector,
                value: "rebeccapurple",
                important: important.checked ? 1 : 0,
                specificity: candidateSelector.specificity,
                sourceOrder: candidateComesLater ? 2 : 0,
            };
            const fallback = {
                selector: "button.action",
                value: "slate",
                important: 0,
                specificity: 11,
                sourceOrder: 1,
            };
            const winner = compareDeclarations(author, fallback) >= 0 ? author : fallback;
            if (specificityValue) specificityValue.textContent = author.selector;
            if (sourceOrderValue) sourceOrderValue.textContent = candidateComesLater ? "yes" : "no";
            if (target) target.style.setProperty("--cascade-color", winner.value);
            output.innerHTML = [
                candidateComesLater
                    ? cssRule(fallback, winner === fallback) + cssRule(author, winner === author)
                    : cssRule(author, winner === author) + cssRule(fallback, winner === fallback),
                `<div class="cascade-winner">winner: <code>${winner.selector}</code> -> <code>color: ${winner.value}</code></div>`,
            ].join("");
        }

        function updateAfterControlEvent() {
            update();
            setTimeout(update, 0);
        }

        [specificity, sourceOrder, important].forEach(control => {
            control.addEventListener("input", updateAfterControlEvent);
            control.addEventListener("change", updateAfterControlEvent);
            control.addEventListener("click", updateAfterControlEvent);
        });
        update();
    }

    function setupComputedDemo() {
        const font = document.querySelector("[data-font-size]");
        const box = document.querySelector("[data-border-box]");
        const output = document.querySelector("[data-em-output]");
        if (!font || !box || !output) return;

        function update() {
            const px = Number(font.value);
            const border = px * 0.8 + 2;
            box.style.setProperty("--demo-font-size", `${px}px`);
            box.style.setProperty("--demo-border-width", `${border}px`);
            output.textContent = `font-size: ${px}px; calc(0.8em + 2px) -> ${border.toFixed(1)}px`;
        }

        font.addEventListener("input", update);
        update();
    }

    function setupLayoutDemo() {
        const width = document.querySelector("[data-container-width]");
        const stage = document.querySelector("[data-layout-stage]");
        const output = document.querySelector("[data-layout-output]");
        if (!width || !stage || !output) return;

        function update() {
            const container = Number(width.value);
            const child = Math.min(container * 0.5, 400);
            stage.style.setProperty("--container-width", `${container}px`);
            output.textContent = `container: ${container}px; child width: min(50%, 400px) -> ${child}px`;
        }

        width.addEventListener("input", update);
        update();
    }

    setupGrammarDemo();
    setupCascadeDemo();
    setupComputedDemo();
    setupLayoutDemo();
    const initialIndex = parseHash();
    if (initialIndex > 0) showSlide(initialIndex, true);
    else {
        updateChrome(false);
        updateSourceZoomGeometry();
    }
    window.addEventListener("resize", updateSourceZoomGeometry);
    window.addEventListener("load", updateSourceZoomGeometry);
    document.querySelectorAll("img").forEach(image => image.addEventListener("load", updateSourceZoomGeometry));
})();
