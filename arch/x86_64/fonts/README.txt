Aileron-Regular.ttf: Aileron Regular by Sora Sagano (dotcolon.net/fonts/aileron), CC0 1.0 (public domain).
This is the ASCII subset that Pillow bundles as ImageFont.load_default() (Pillow 12.3.0, base64 in PIL/ImageFont.py),
the same face gen_uifont.py rasterizes into kernel/uifont.h. Aurora (aurora_text.c) renders it with FreeType at
scaled sizes so the shell stays crisp at 125-400 %. SHA-256 69853909b940023570964e29cffe30da95aea8de3627736b5cd15ab30143169f,
12676 bytes. The build embeds it as out/aurora_font_data.h (desktop.py font_header()).
