# CSS Value Processing End To End

This document is the general CSS model for the talk. It follows MDN's "CSS property value processing" terminology and maps it to browser-engine implementation concerns.

Primary reference:

- https://developer.mozilla.org/en-US/docs/Web/CSS/Guides/Cascade/Property_value_processing

## One Sentence

CSS value processing is the sequence that turns a syntactically valid declaration into the value a browser can inherit, lay out, paint, serialize to JavaScript, and finally approximate on the screen.

For a frontend audience, frame this as "the backstage route from the CSS you wrote to the thing DevTools, layout, and pixels can observe."

## The Pipeline

```text
stylesheet text
  -> tokens and component values
  -> valid declarations
  -> declared values
  -> cascaded value
  -> specified value
  -> computed value
  -> used value
  -> actual value
```

The six spec value stages are declared, cascaded, specified, computed, used, and actual. Stylesheet text and parsing happen before those stages. CSSOM resolved values are a separate web-facing view: APIs such as `getComputedStyle()` may expose computed values for some properties and used values for others.

Talk hook: authors see one declaration; the browser sees a queue of questions.

In the deck, this abstract pipeline is later mapped onto Ladybird concepts: parser declarations, `StyleProperty`, `CascadedProperties`, `StyleComputer`, `ComputedProperties`, layout, and painting/compositing. `CSSStyleProperties` is shown separately as the CSSOM view over those results.

## Filtering

The browser starts with style sources:

- user-agent stylesheets;
- user stylesheets, if supported;
- author stylesheets;
- inline style attributes;
- presentational hints;
- animation and transition origins.

Only some declarations apply to a given element:

- the stylesheet must apply to the document;
- conditional rules such as `@media`, `@supports`, `@container`, and `@scope` must match;
- the selector must match the element;
- the property name must be recognized;
- the value must match that property's accepted syntax.

Declarations that fail these filters do not become declared values.

Frontend hook: this is why CSS feels resilient. One unsupported experiment can fail without making the rest of the file unusable.

## Declared Value

A declared value is a syntactically valid value from a declaration that applies to the element.

An element can have zero, one, or many declared values for the same property:

```css
p {
    color: black;
}

.warning {
    color: red;
}
```

For `<p class="warning">`, both declarations are declared values for `color`.

## Cascaded Value

The cascaded value is the declared value that wins the cascade.

The cascade compares declarations by several axes, including:

- relevance;
- origin and importance;
- cascade layer;
- specificity;
- scoping proximity;
- source order.

There is at most one cascaded value for a property on an element. If nothing wins because no declaration applies, there is no cascaded value.

Frontend hook: "last wins" is a shortcut. It only works after relevance, origin, importance, layers, specificity, and scope have already had their say.

## Specified Value

The specified value is the result of defaulting. It always exists for every property on every element.

Rules:

1. If there is a cascaded value, it becomes the specified value.
2. If there is no cascaded value and the property is inherited, the specified value is the parent's computed value.
3. If there is no cascaded value and the property is not inherited, the specified value is the property's initial value.

CSS-wide keywords affect this step:

- `inherit`: use the inherited value.
- `initial`: use the property's initial value.
- `unset`: inherit if inherited, otherwise initial.
- `revert`: roll back to the value from a previous cascade origin.
- `revert-layer`: roll back within the cascade layer stack.

Frontend hook: these keywords are not just values; they are instructions to run parts of the defaulting algorithm.

## Computed Value

The computed value is the value transferred from parent to child during inheritance.

It is calculated from the specified value by:

- resolving CSS-wide keywords;
- following the "computed value" line in the property's specification;
- resolving many relative values that do not need layout;
- substituting custom properties where needed;
- converting some keyword or relative forms into more absolute forms.

Examples:

```css
.box {
    font-size: 20px;
    padding-top: 2em;
}
```

The computed `padding-top` is `40px`, because `em` can resolve from the computed font size without layout.

Not everything resolves at computed-value time:

```css
.item {
    width: 50%;
}
```

The computed value may still be a percentage because the actual pixel width depends on the containing block during layout.

Frontend hook: this is where "but DevTools showed pixels" starts to become a compatibility story rather than a pure computed-value story.

## Custom Properties

Custom properties are unusual because their raw token streams cascade first and may be substituted later.

```css
.box {
    --space: 2em;
    padding: var(--space);
}
```

`--space` wins the cascade as a custom property. `padding` then substitutes `var(--space)`. If substitution produces a value invalid for `padding`, the declaration can become invalid at computed-value time.

That is why custom properties cannot be treated as normal parse-time constants.

Frontend hook: `var()` lets CSS defer meaning. That power is also why some failures arrive fashionably late.

## Used Value

The used value is the value after layout-dependent calculations are done.

Used values require information such as:

- containing block size;
- writing mode;
- font metrics;
- intrinsic image or video dimensions;
- available inline size;
- min/max constraints;
- line breaking;
- table, flex, grid, or block formatting context rules.

Examples:

```css
.box {
    width: 50%;
    max-width: 400px;
}
```

The computed width may remain `50%`; the used width is the actual pixel width after the containing block and max constraint are known.

## Actual Value

The actual value is the used value after approximations imposed by the output environment.

Examples:

- border widths may be rounded to device pixels;
- fonts may be substituted;
- subpixel geometry may be snapped or accumulated differently by the renderer;
- color output may be transformed by the target surface.

This is close to rendering, but it is not the same as saying "the final pixels". It is still the CSS value after the used value has been adjusted for output-environment limits.

## Resolved Value

The resolved value is what APIs such as `getComputedStyle()` expose.

Despite the API name, `getComputedStyle()` does not always return the internal computed value. For web compatibility, many properties return the computed value, while some layout-dependent legacy properties return used values.

This distinction matters in demos:

```js
getComputedStyle(element).width
```

Often returns a pixel value, even if the authored declaration was `width: 50%`.

There are surprises in the other direction too:

```css
line-height: normal;
```

For `line-height`, Ladybird follows the CSSOM resolved-value rule: `normal` stays `normal`, while non-`normal` values can resolve through used font metrics.

Frontend hook: `getComputedStyle()` is an API with web-compat baggage. Useful baggage, but baggage.

## Why Engines Separate These Stages

Different stages answer different questions:

- Parser: "Is this declaration valid syntax?"
- Cascade: "Which declaration wins?"
- Defaulting: "What value exists if nothing wins?"
- Computation: "What can be resolved without layout?"
- Layout: "What is the geometry?"
- Paint: "What should be drawn?"
- CSSOM: "What must JavaScript observe?"

Keeping the stages separate makes it easier to match CSS specifications, avoid unnecessary layout, cache style data, invalidate only what changed, and explain interop bugs.

## Compact Example

```html
<article class="card">
    <h1>CSS Day</h1>
</article>
```

```css
article {
    font-size: 20px;
}

.card {
    width: 50%;
    padding-left: calc(2em + 8px);
    color: rebeccapurple;
}
```

For `.card`:

- declared values include `width: 50%`, `padding-left: calc(2em + 8px)`, `color: rebeccapurple`, and any user-agent defaults;
- cascaded values are the winners for each property;
- specified values include those winners plus inherited or initial values for every other property;
- computed `padding-left` can become `48px`;
- computed `width` can remain percentage-like;
- used `width` becomes a pixel size during layout;
- actual width may be rounded for the output device;
- resolved `getComputedStyle(card).width` likely returns a pixel value.

That is the whole talk in miniature.
