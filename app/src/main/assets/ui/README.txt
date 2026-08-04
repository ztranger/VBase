# UI skin textures (RGBA PNG) for UiSkin

Guides:
  docs/UI_SKIN.md     — 9-slice geometry, sizes, checklist
  docs/UI_PALETTE.md  — TD × Orcs Must Die color palette

Files:
  panel.png            — 9-slice window (border = 25% of min side; warm stone)
  button_normal.png    — amber CTA idle
  button_hover.png     — amber hover
  button_active.png    — amber pressed
  loading_core.png     — loading art (base core in niche, 16:9)
  loading_glow.png     — soft radial bloom (gen_loading_fx.py)
  loading_beam.png     — soft god-ray (gen_loading_fx.py)
  loading_mote.png     — dust mote (gen_loading_fx.py)

Regenerate UI chrome: python gen_ui_skin.py
Regenerate loading FX:  python gen_loading_fx.py
