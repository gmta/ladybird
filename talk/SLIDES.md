# CSS Day 2026 Slide Outline

Working title: **CSS, From Text to Pixels in Ladybird**

Target length: 50 minutes, including short live moments. The deck should be personal, technical, and CSS-first: the audience should feel like they got a backstage tour of browser machinery, with Ladybird as the surprising browser running the tour.

## Audience And Tone

The room is mostly frontend developers who are enthusiastic about CSS features. Do not open with an abstract browser-engine thesis. Start with a human reason for the talk:

- I am Jelle Raaijmakers, COO of the Ladybird Browser Initiative.
- Ladybird is a 501(c)(3) non-profit based in San Francisco, 100% supported by donations and sponsorships.
- We are building a new browser engine from scratch, based on web standards, written mostly in C++ and Rust.
- This slide deck is HTML/CSS/JS and is intended to run inside Ladybird itself.
- Use real Ladybird source excerpts on the value-processing slides, but keep them short and heavily summarized.

Tone: personal, curious, light-hearted, and precise. The humor can be chosen later, but the structure should leave room for it. The talk should not feel like a marketing pitch; Ladybird is the setting and the proof point, while CSS value processing is the subject.

Diagram and panel boxes should read like labels, not prose paragraphs: do not end boxed text with a period.

## Main Takeaways

1. There is a lot of machinery behind CSS.
2. There is a new browser in town, and it is far enough along to present a CSS Day talk from inside it.

## Recurring Visual Motif

Use a CSS value-processing pipeline repeatedly:

```text
stylesheet text -> Declared -> Cascaded -> Specified -> Computed -> Used -> Actual
```

Show stylesheet text as the input, then make the six spec value stages the recurring visual pipeline. Treat resolved values and `getComputedStyle()` as a separate CSSOM view that may read from computed values or used values, depending on the property.

Use `width: 50%` as the first example because it is readable and introduces layout at the used-value stage:

- **Declared value:** a valid declaration that applies to an element.
- **Cascaded value:** the winning declaration for `width`.
- **Specified value:** the cascaded value, or the initial value `auto` if nothing wins.
- **Computed value:** the percentage `50%`.
- **Used value:** the pixel width after layout knows the containing block.
- **Actual value:** the used value after output-environment approximations, such as rounding or device constraints.

Then show resolved values separately: what CSSOM exposes for `getComputedStyle()`, often computed values, sometimes used values for web compatibility.

Use more context-sensitive examples later only after this simple pipeline is clear.

## Structure

| # | Time | Slide | Purpose |
|---|------|-------|---------|
| 0 | 0:15 | Ladybird intro animation | Official animation, starts only on presenter input; the next presenter input advances. |
| 1 | 0:45 | Title: CSS, From Text to Pixels in Ladybird | Establish the subject. The live Ladybird context can be mentioned verbally. |
| 2 | 1:00 | Jelle Raaijmakers | Personal intro: COO of the Ladybird Browser Initiative, building a new browser with a team. |
| 3 | 0:45 | Ladybird: new engine, not a fork | New independent browser engine, from scratch, web-spec driven, written mostly in C++ and Rust, backed by a 501(c)(3), 100% supported by donations and sponsorships. |
| 4 | 1:00 | Before Ladybird was Ladybird | Three-image history beat: the browser growing inside SerenityOS, the real web starting to show up, and Acid3 as an early milestone. |
| 5 | 0:30 | Synthetic tests & the real web | Position WPT and Test262 results as proof points for basic capability while making clear the goal is the real web; show both charts side by side with scores measured on June 10, 2026. |
| 6 | 0:10 | Well, in fact... | Reveal beat: the whole deck is running inside Ladybird. |
| 7 | 1:00 | Cooperating processes | Full process diagram: UI/browser process, WebContent as the web platform process, and sandboxed processes such as RequestServer, ImageDecoder, WebWorker, Compositor, grouped as connected through IPC. |
| 8 | 0:45 | CSS inside LibWeb | Zoom into WebContent/LibWeb and show CSS as one part of LibWeb alongside DOM, layout, painting, JavaScript, and the wider set of web specifications. |
| 9 | 0:45 | The page pipeline around CSS | Page pipeline diagram: fetch -> parse -> DOM/CSSOM -> style -> layout -> paintable tree -> display list -> composite. |
| 10 | 1:00 | One CSS value, many stages | Introduce the core question with `width: 50%` and keep the slide terse: "Simple enough, right?" |
| 11 | 2:00 | Six CSS value stages | Show stylesheet text feeding the six spec value stages; keep CSSOM resolved values out of the pipeline graphic. |
| 12 | 0:45 | Stylesheet text | Show the text before the six-stage pipeline begins. |
| 13 | 1:15 | Declared values: valid declarations | Stage 1 highlight: valid declarations that apply; invalid CSS is filtered out. |
| 14 | 1:30 | Property metadata | `Properties.json` stores initial value, inheritance, grammar, animation type, computation and invalidation facts. |
| 15 | 2:00 | Parser as filter | Demo accepts/rejects simple values; explain this as the parser's contribution to declared values. |
| 16 | 1:00 | The parser gate in LibWeb | Show the parser gate in source: property lookup plus `parse_css_value`; note that the demo uses `CSS.supports(property, value)`. |
| 17 | 1:15 | Cascaded value: one winner | Stage 2 highlight: many declarations, one winner. |
| 18 | 2:30 | One property, one winner | Show two real-looking CSS rules targeting `button.action`; change selector specificity, source placement, and `!important` to see which declaration wins; include the `StyleComputer::compute_cascaded_values()` code that applies declarations in cascade order. |
| 19 | 1:15 | Specified values: blanks filled | Stage 3 highlight: defaulting guarantees a value even when nothing cascades; show the `StyleComputer` branch that chooses inherited or initial values. |
| 20 | 1:00 | CSS-wide keywords | `inherit`, `initial`, `unset`, `revert`, `revert-layer` as instructions to defaulting/cascade. |
| 21 | 1:15 | Computed values before layout | Stage 4 highlight: resolve what can be resolved without layout; show where `StyleComputer` absolutizes specified values with a computation context. |
| 22 | 2:30 | `calc()` and its available inputs | A font-size input affects the computed left border width for `calc(0.8em + 2px)`; contrast with percentage values that still need layout. |
| 23 | 1:15 | Used values with layout context | Stage 5 highlight: layout-dependent values need box/tree context. |
| 24 | 2:00 | Percentages and geometry | `width: 50%; max-width: 400px` turns into pixels with a container. |
| 25 | 1:00 | Actual values and the output device | Stage 6 highlight: used values adjusted for output-environment limitations such as rounding, device pixels, and font substitution. |
| 26 | 1:45 | `getComputedStyle()` and resolved values | CSSOM epilogue: resolved values are the API-facing view; show `line-height: normal` coming from computed data and `width: 50%` coming from used/layout data. |
| 27 | 1:45 | Resolved values as a CSSOM decision | Show why `getComputedStyle()` sometimes needs layout and sometimes serializes computed style directly. |
| 28 | 1:30 | `width: 50%`, stage by stage | Walk the example end to end as a vertical pipeline, with stage names in one column and stage-specific values beside them. |
| 29 | 1:30 | `padding-left`, stage by stage | Repeat the vertical pipeline with `revert-layer`, `var()`, `em`, percentages, layout, and device-pixel snapping so the stages visibly change. |
| 30 | 1:30 | Context-sensitive values | Show why `width`, `em`, `currentColor`, and `var()` add dependencies. |
| 31 | 1:30 | Custom properties and deferred substitution | Substitution can make a declaration invalid at computed-value time. |
| 32 | 1:00 | Style storage changes shape | Show how Ladybird stores style differently at different points: declared lists, sparse cascade state, dense computed longhand storage, typed layout data, and shared custom-property chains. |
| 33 | 1:00 | Sparse, dense, shared | Show source-backed storage details: temporary sparse `CascadedProperties`, dense `ComputedProperties`, shared custom-property chains, and what can be discarded after style computation. |
| 34 | 1:30 | Time-travelling `font-size` | Show the monospace font-size quirk: a later `font-family: monospace` result makes Ladybird walk back through ancestor `font-size` values and reinterpret keyword/relative sizes against the monospace default. |
| 35 | 1:15 | Ladybird concepts for the six stages | Map the six value-processing stages to concrete Ladybird concepts: parser declarations, `CascadedProperties`, `StyleComputer`, `ComputedProperties`, layout, and painting/compositing. |
| 36 | 1:30 | Specified and computed values in `StyleComputer` | Show a reduced loop for cascaded/specified/computed value handling. |
| 37 | 2:30 | `@container` and the page tree | Show `@container` and `container-type` as a feature where parser, cascade, computed style, ancestor selection, writing mode, and layout context all interact; keep the source excerpt valid C++-style pseudocode. |
| 38 | 1:00 | CSS as a language with a runtime | CSS is a language with a runtime, not just a stylesheet format; include "Brought to you today by Ladybird, a brand-new browser." |
| 39 | 0:30 | Thank you | Close cleanly with the Ladybird logomark. |

Total planned speaking time: about 50 minutes.

## Demo Slides To Build Out

- **Grammar demo:** accept/reject common values for a small set of properties.
- **Cascade demo:** choose `!important`, selector specificity, and source order.
- **Computed/calc demo:** change `font-size` and watch `calc(0.8em + 2px)` resolve while percentage values wait for layout.
- **Layout demo:** change container width and watch a percentage value resolve.

## Visual Direction

- Dark page, subtle section bands, purple accent, muted text, compact monospace controls.
- Use Ladybird.org's restraint: large direct type, minimal chrome, clear sections.
- Use CSS diagrams rather than screenshots for the first pass.
- Use the same pipeline component repeatedly, with one highlighted stage at a time.
- Syntax-highlight CSS examples as well as C++/JSON source excerpts.
- Keep controls simple enough to run in Ladybird: no external dependencies, no build step, no CDN.
