---
applyTo: "Apps/**/icon.svg,Apps/**/icon_mono.svg"
---

# Tactility launcher icons

- Follow the [Tactility icon guide](https://github.com/TactilityProject/TactilityDocs/blob/main/docs/main/app-development/icons.md).
- Provide both color and monochrome variants at `96` by `96` with
  `viewBox="0 0 96 96"`. Use simple SVG shapes supported by the launcher.
- The color icon has a flat, full-size background and a high-contrast glyph.
  The monochrome icon is a black glyph on transparency, without a background
  tile or border; cut transparent holes with `fill-rule="evenodd"`.
- Center the monochrome glyph, roughly 80 units across. Keep strokes at least
  3-4 units wide and avoid tiny details, text, patterns, filters, and masks.
- Render and inspect both variants at 96 and 24 pixels where tooling permits,
  and check them on a device or simulator when available.
