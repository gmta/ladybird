# Ladybird CSS Property Implementation Notes

This is a working reference for the CSS Day 2026 deck. It focuses on how CSS properties move through Ladybird's LibWeb implementation, not on the entire browser engine.

The audience framing should be frontend-friendly: each implementation detail should answer a question a CSS author might actually ask in DevTools, in a bug reduction, or while trying a new feature.

## Big Picture

Ladybird is written mostly in C++ and Rust. The CSS implementation discussed in this deck lives primarily in LibWeb's C++ code today, with the broader browser split across cooperating, sandboxed processes.

Ladybird's CSS implementation is data-driven where possible and hand-written where necessary.

The main path is:

1. Property metadata lives in `Libraries/LibWeb/CSS/Properties.json`.
2. Generated code turns that metadata into enums and helper functions.
3. The parser turns CSS text into `StyleProperty` entries containing `StyleValue` objects.
4. `StyleComputer` matches rules, performs the cascade, resolves custom properties, defaults missing values, and computes values.
5. `ComputedProperties` stores per-element longhand values as `StyleValue` pointers plus metadata such as importance, inheritance, and animation state.
6. Layout-facing code converts many of those values into compact `ComputedValues`.
7. CSSOM APIs serialize declared, computed, or resolved values back to JavaScript.

## Talk Stage Map

The deck maps the web-facing CSS value-processing stages to Ladybird concepts like this:

- **Declared:** parser output such as `Parser::Declaration` and `StyleProperty`.
- **Cascaded:** `CascadedProperties` entries, including origin, layer, source, and winning order.
- **Specified:** `StyleComputer` choosing cascaded, inherited, initial, and CSS-wide keyword behavior.
- **Computed:** `ComputedProperties` storing computed longhand `StyleValue` objects.
- **Used:** layout resolving percentages, constraints, box geometry, and font metrics.
- **Actual:** painting and compositing adapting used values to device output.

Stylesheet text and style attributes feed into the declared-value stage. `CSSStyleProperties` is the CSSOM-facing layer after the six stages; it serializes values for APIs such as `getComputedStyle()` and may need layout for compatibility-dependent resolved values.

## Property Metadata

`Libraries/LibWeb/CSS/Properties.json` describes each supported property. Important fields include:

- `initial`: the property's initial value.
- `inherited`: whether defaulting inherits from the parent.
- `valid-identifiers`: accepted keyword values.
- `valid-types`: accepted CSS value grammar types.
- `longhands`: shorthand expansion targets.
- `animation-type`: how Web Animations can interpolate or combine the property.
- `requires-computation`: when specified values must be converted into computed values.
- `affects-layout`: whether changes should invalidate layout.
- `needs-layout-for-getcomputedstyle`: whether resolved-value queries need layout.

`Documentation/CSSGeneratedFiles.md` documents this generated layer. It keeps common property facts centralized so parsing, CSSOM, invalidation, and computation do not drift apart.

Speaker framing: this is the browser's cheat sheet for each CSS property. If a property can inherit, animate, parse a color, expand into longhands, or trigger layout, the engine needs a reliable place to know that.

## Generated Property Facts

The build generates C++ files from CSS JSON metadata. These generated helpers include:

- `PropertyID` and property-name mapping.
- `property_initial_value(PropertyID)`.
- `property_accepts_keyword(PropertyID, Keyword)`.
- `property_accepts_type(PropertyID, ValueType)`.
- `longhands_for_shorthand(PropertyID)`.
- `is_inherited_property(PropertyID)`.
- `property_requires_computation_*`.
- `property_needs_layout_for_getcomputedstyle(PropertyID)`.
- Keyword and enum conversion helpers.

The useful talk framing: Ladybird does not want every CSS subsystem to know how `width`, `font-size`, or `mask-image` works by folklore. Property facts should be queryable.

## Parsing

The parser entry points live under `Libraries/LibWeb/CSS/Parser/`.

`Parser::parse_as_css_value(PropertyID)` parses one property value. For many properties, `PropertyParsing.cpp` uses generated metadata:

- If the property accepts a keyword, `property_accepts_keyword()` validates it.
- If the property accepts a CSS value type, the parser dispatches to the matching value parser.
- If the property is a shorthand, parser code returns a `ShorthandStyleValue`.
- If the syntax is special, a hand-written parser handles it.

Parsed values are represented as subclasses of `StyleValue`, such as keyword, length, percentage, color, list, function, transformation, shorthand, unresolved, and calculated values.

Parsing filters out invalid declarations early. Invalid property names or values do not become declared values for the cascade.

Speaker framing: this is why CSS can be forgiving. A bad declaration does not usually poison the whole stylesheet; it just never gets a ticket into the cascade.

## Cascaded Properties

`StyleComputer::compute_cascaded_values()` performs the cascade into a `CascadedProperties` object.

The cascade order includes:

- normal user-agent declarations;
- normal user declarations;
- author presentational hints;
- normal author declarations, including layers and shadow-tree contexts;
- inline author style;
- important author declarations;
- important user declarations;
- important user-agent declarations.

Transitions are not applied in this phase; they are handled after style computation.

`CascadedProperties` stores vectors of entries per property. Each entry records the `StyleProperty`, cascade index, origin, layer name, source declaration, and source shadow root. The last entry for a property is the winning cascaded value.

Speaker framing: the cascade is not "the last rule wins"; it is "the last rule wins after a long list of more important questions."

## Style Storage Lifecycle

The deck now uses style storage as an implementation story:

- Declared style lives on CSS rules and inline style as `StyleProperty` entries.
- Cascaded style is stored temporarily in `CascadedProperties`, a sparse map keyed by property ID.
- Computed style is stored on the element as `ComputedProperties`, with a dense array of longhand `StyleValue` pointers and compact bitsets for flags.
- Layout-facing style is copied into typed `ComputedValues` so layout and painting can read fast, structured data.
- Custom properties use `CustomPropertyData`, a structurally shared parent chain, instead of copying the full inherited custom-property map onto every element.

The important memory distinction: the cascade is an intermediate decision object. The page keeps computed style, layout style, and custom-property data, but it does not need to keep the full cascade object alive after style computation.

Speaker framing: CSS starts as flexible author text, but browser storage gets stricter as the engine gets closer to layout.

## Custom Properties

Custom properties are resolved before regular properties are fully computed.

`StyleComputer::compute_style_impl()` collects cascaded custom properties, compares them with inherited parent custom-property data, and stores compact `CustomPropertyData`.

`CustomPropertyData` stores only values declared on the current element, plus a parent pointer to inherited data. If the chain gets too deep, it is flattened. If a parent has only a few values, they can be absorbed to shorten lookups. Inheritable views are cached per document and custom-property registration generation.

Substitution can produce unresolved or guaranteed-invalid values. This matters because custom properties can make a declaration invalid at computed-value time, not just parse time.

Good talk example:

```css
.box {
    --gap: 2em;
    padding: var(--gap);
}
```

`--gap` cascades as its own property, then `padding` receives the substituted value and computes relative to the element's font metrics.

Speaker framing: custom properties are where CSS keeps suspense in the story. A declaration can look fine until substitution reveals what it really means.

## Specified To Computed

After the cascade, Ladybird fills a `ComputedProperties` object.

The specified value for a longhand comes from:

- the cascaded value, if one exists;
- otherwise the inherited computed value, for inherited properties;
- otherwise the property's initial value.

Then `StyleComputer::compute_property_values()` iterates in `property_computation_order()`. For each property, it builds a `ComputationContext` and calls:

```c++
NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_property(
    PropertyID,
    NonnullRefPtr<StyleValue const> const& specified_value,
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value,
    ComputationContext const&,
    double device_pixels_per_css_pixel);
```

Computation handles work such as:

- font-relative length resolution;
- color and `currentColor` handling;
- `line-height` rules;
- font-size keyword and relative-size mapping;
- border/outline width keyword conversion;
- value-specific cleanup for complex properties;
- viewport-metric dependency tracking.

Computation order matters because properties depend on one another. Font-related values must be ready before resolving `em` lengths; color-scheme must be known before resolving system colors.

## Time-Travelling Font Size

Ladybird has a special compatibility path for `font-family: monospace`.

When an element's cascaded `font-family` is exactly `monospace`, other browsers reinterpret keyword and relative `font-size` values in the ancestor chain as if the default font size were the monospace default, 13px, instead of the normal default, 16px.

Example:

```css
html { font-size: medium; }
body { font-size: 200%; }
main { font-size: 2em; }
article { font-family: monospace; }
```

Without the monospace quirk, this reads as:

```text
16px -> 32px -> 64px
```

With the monospace quirk, Ladybird walks back through the inheritance chain and recomputes raw winning `font-size` values:

```text
13px -> 26px -> 52px
```

This is called time-travelling inheritance because a later cascaded result, `font-family: monospace`, changes how earlier inherited `font-size` values should have been interpreted.

Implementation notes:

- `ComputedProperties` keeps `m_raw_cascaded_font_size` specifically for this path.
- `StyleComputer::recascade_font_size_if_needed()` checks the cascaded `font-family`.
- If it is exactly `monospace`, Ladybird reconstructs the ancestor chain and recomputes only `font-size`.
- The code intentionally does not trigger for `font-family: monospace, AnythingElse`.

## ComputedProperties

`ComputedProperties` stores longhand `StyleValue` pointers in a flat array-like structure keyed by `PropertyID`.

It also tracks:

- whether a property was important;
- whether it was inherited;
- animated property values;
- whether a value depends on viewport metrics;
- cached font lists.

It exposes higher-level getters that convert raw `StyleValue` objects into engine-friendly forms:

- `display()`;
- `font_size()`;
- `background_layers()`;
- `border_spacing_horizontal()`;
- `transformations()`;
- `color(PropertyID, ColorResolutionContext)`;
- many others.

The name can be misleading for web developers: this is internal computed style, not necessarily what `getComputedStyle()` serializes.

Speaker framing: DevTools and CSSOM are real, but they are not a window directly into every internal engine object.

## Layout-Facing ComputedValues

Layout nodes use `ComputedValues`, a compact representation declared in `ComputedValues.h`.

`Layout::NodeWithStyle::apply_style()` copies values from `ComputedProperties` into `ComputedValues`. It handles ordering constraints explicitly:

- color-scheme first, so system colors resolve correctly;
- font list, font size, font weight, and line height early;
- color after font size and color-scheme;
- then layout, paint, transform, SVG, and other properties.

This step is where many `StyleValue` objects become the typed values layout and painting want to read quickly.

## CSSOM And Resolved Values

`CSSStyleProperties` handles CSSOM serialization and computed style access.

For computed style, `CSSStyleProperties::get_direct_property()` decides how much work a property query requires:

- some properties only need up-to-date style;
- some need a layout node;
- some need layout because the resolved value is layout-dependent;
- logical aliases and shorthands may need special handling.

This is where the web-visible distinction between computed values and resolved values matters. `getComputedStyle()` is historically constrained and may expose used values for properties such as width and height.

Speaker framing: `getComputedStyle()` is named after history, not after the cleanest mental model.

## Implementation Checklist For A New Property

1. Add or update metadata in `Properties.json`.
2. Add keywords or generated enums if needed.
3. Use generated parsing if the grammar is simple.
4. Add hand-written parser support for special grammar.
5. Add or reuse a `StyleValue` representation.
6. Define computation behavior if the specified value differs from computed.
7. Add `ComputedProperties` getters if the engine needs a typed form.
8. Copy into `ComputedValues` if layout or paint needs fast access.
9. Add CSSOM serialization or resolved-value handling if the default is wrong.
10. Add tests: parsing, computed values, layout/paint, CSSOM, and WPT where applicable.

## Good Talk Anchors

- `font-size`: shows inheritance, relative-size computation, and dependency ordering.
- `font-family: monospace`: shows time-travelling inheritance and browser-compatibility machinery.
- `width`: shows specified/computed/resolved/used value differences.
- `color`: shows inheritance, `currentColor`, and color-resolution context.
- custom properties: shows computed-value-time invalidation.
