# UI skin textures (RGBA PNG) for UiSkin

Full guide (sizes, 9-slice margins, checklist):
  docs/UI_SKIN.md

Files:
  panel.png            — 9-slice window frame (border = 25% of min side)
  button_normal.png    — button idle
  button_hover.png     — button hovered
  button_active.png    — button pressed

On 128×128 panel → border strip is 32px on each side; center 64×64 is fill only.

Regenerate procedural stubs: python gen_ui_skin.py
