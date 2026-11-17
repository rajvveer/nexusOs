"""Generate the Nexus Browser test site into <WS>/site (run with python -I gen_site.py <site_dir>).
Pages: index.html (every supported construct + 20+ links + 4 images incl. a 1200 px banner),
long.html (300 paragraphs), big.html (~300 KB), broken.html (bad nesting / unclosed tags),
plain.txt, download.bin, and the images (PNG / JPEG baseline / GIF / RGBA PNG)."""
import os, sys, random
from PIL import Image, ImageDraw

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def w(name, text):
    open(os.path.join(out, name), "w", encoding="utf-8", newline="\n").write(text)

# ---------------------------------------------------------------- images
def checker(img, size, c1, c2):
    d = ImageDraw.Draw(img)
    for y in range(0, img.size[1], size):
        for x in range(0, img.size[0], size):
            d.rectangle([x, y, x + size - 1, y + size - 1], fill=c1 if ((x // size + y // size) % 2 == 0) else c2)

im = Image.new("RGB", (64, 64)); checker(im, 8, (220, 40, 40), (255, 220, 220))
ImageDraw.Draw(im).ellipse([12, 12, 51, 51], outline=(20, 20, 80), width=3)
im.save(os.path.join(out, "small.png"))

im = Image.new("RGB", (320, 200), (30, 90, 160))
d = ImageDraw.Draw(im)
for i in range(0, 320, 20): d.line([i, 0, 319 - i, 199], fill=(255, 255, 255))
d.rectangle([40, 60, 280, 140], fill=(250, 200, 60)); d.text((120, 92), "JPEG 320x200", fill=(0, 0, 0))
im.save(os.path.join(out, "photo.jpg"), quality=85, subsampling=0)  # baseline

frames = []
for f in range(4):
    g = Image.new("P", (120, 80))
    g.putpalette(sum([[i, 255 - i, (i * 3) % 256] for i in range(256)], []))
    gd = ImageDraw.Draw(g)
    gd.rectangle([0, 0, 119, 79], fill=40 + f * 50)
    gd.ellipse([20 + f * 15, 20, 60 + f * 15, 60], fill=200)
    frames.append(g)
frames[0].save(os.path.join(out, "anim.gif"), save_all=True, append_images=frames[1:], duration=200, loop=0)

im = Image.new("RGB", (1200, 180), (245, 245, 250))
d = ImageDraw.Draw(im)
for i in range(0, 1200, 60):
    d.rectangle([i, 0, i + 29, 179], fill=(90 + (i // 60) * 7, 120, 200))
d.rectangle([0, 0, 1199, 179], outline=(0, 0, 0), width=4)
d.text((20, 80), "1200 x 180 banner - should be scaled to the content width", fill=(0, 0, 0))
im.save(os.path.join(out, "banner.png"))

im = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
d = ImageDraw.Draw(im); d.ellipse([4, 4, 91, 91], fill=(40, 180, 90, 255)); d.text((30, 42), "RGBA", fill=(255, 255, 255, 255))
im.save(os.path.join(out, "alpha.png"))

# ---------------------------------------------------------------- pages
links = "".join(f'<li><a href="/page{i}.html">Link number {i}</a></li>\n' for i in range(1, 23))
index = f"""<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>Nexus Browser Test Page</title>
<style>body {{ color: red; }}</style>
<script>document.write("<h1>SCRIPT MUST NOT RENDER</h1>");</script>
</head>
<body>
<!-- a comment that must not show -->
<h1>Heading level 1</h1>
<h2>Heading level 2</h2>
<h3>Heading level 3</h3>
<h4>Heading level 4</h4>
<h5>Heading level 5</h5>
<h6>Heading level 6</h6>
<p>This is a <b>bold</b> word, an <i>italic</i> word, an <u>underlined</u> word, <strong>strong</strong>,
<em>emphasised</em> and some <code>inline code()</code>. Entities: &amp; &lt;tag&gt; &quot;quoted&quot; &copy; 2026 &mdash; dash &hellip; &#65;&#x42;C &nbsp;spaced.
UTF-8: café — “quotes” • bullet. A long paragraph follows so that the inline text wrapping can be checked
against the right edge of the window: the quick brown fox jumps over the lazy dog, the quick brown fox jumps over the lazy dog,
the quick brown fox jumps over the lazy dog, the quick brown fox jumps over the lazy dog.</p>
<p align="center">A centred paragraph with a <a href="/long.html">link to the long page</a> and <font color="#c02020">red text</font>.</p>
<hr>
<h2>Images</h2>
<p>Small PNG: <img src="small.png" alt="small png" width="64" height="64"> JPEG (natural size): <img src="photo.jpg" alt="photo"> Animated GIF: <img src="anim.gif" alt="gif"> RGBA: <img src="/alpha.png" alt="alpha"> Missing: <img src="/nope.png" alt="missing image" width="100" height="40"></p>
<p>Wide banner (1200 px, must shrink to fit):</p>
<img src="banner.png" alt="banner">
<h2>Lists</h2>
<ul>
<li>First bullet</li>
<li>Second bullet with a nested list
  <ul><li>nested one</li><li>nested two <a href="/plain.txt">plain text link</a></li></ul>
</li>
<li>Third bullet</li>
</ul>
<ol>
<li>Ordered one</li><li>Ordered two</li><li>Ordered three</li>
</ol>
<h2>Table</h2>
<table border="1">
<tr><th>Name</th><th>Type</th><th>Size</th></tr>
<tr><td>small.png</td><td>PNG</td><td>64 x 64</td></tr>
<tr><td>photo.jpg</td><td>JPEG</td><td>320 x 200</td></tr>
<tr><td>banner.png</td><td>PNG</td><td>1200 x 180</td></tr>
</table>
<blockquote>A blockquote: &ldquo;The Web does not just connect machines, it connects people.&rdquo; &mdash; Tim Berners-Lee</blockquote>
<pre>preformatted   text
    keeps   spacing
and line breaks &lt;ok&gt;</pre>
<h2>Twenty-two links</h2>
<ol>
{links}</ol>
<p>Special: <a href="/redirect">redirect to index</a> | <a href="/big.html">300 KB page</a> | <a href="/broken.html">broken html</a> |
<a href="/download.bin">download</a> | <a href="/photo.jpg">image alone</a> | <a href="relative/../small.png">dot-dot relative</a> |
<a href="about:home">about:home</a></p>
<p><small>Small footer text.</small> <span>span text</span> <noscript>NOSCRIPT MUST NOT RENDER</noscript></p>
</body>
</html>
"""
w("index.html", index)

paras = "\n".join(f"<p>Paragraph {i}: the quick brown fox jumps over the lazy dog while the browser scrolls through three hundred paragraphs of text to prove that the layout, the scrollbar and the keyboard scrolling all work together. Word {i * 7 % 101}.</p>" for i in range(1, 301))
w("long.html", f"<html><head><title>Long page (300 paragraphs)</title></head><body><h1>Long page</h1>\n{paras}\n<p><a href='/index.html'>Back to index</a></p><h2>The end</h2></body></html>\n")

random.seed(7)
words = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua".split()
chunks = ["<html><head><title>Big page (300 KB)</title></head><body><h1>Big page</h1>\n"]
size = len(chunks[0]); i = 0
while size < 300 * 1024:
    i += 1
    p = f"<p>Block {i}: " + " ".join(random.choice(words) for _ in range(60)) + "</p>\n"
    chunks.append(p); size += len(p)
chunks.append("<p><b>END MARKER - should not be visible if truncated at 256 KB</b></p></body></html>\n")
w("big.html", "".join(chunks))

w("broken.html", """<html><head><title>Broken HTML<body>
<h1>Unclosed heading
<p>Paragraph one without a close tag
<p>Paragraph <b>two with bold <i>and italic</b> wrongly nested</i> text.
<ul><li>item one<li>item two<ul><li>nested<li>nested 2</ul><li>item three
<table><tr><td>cell a<td>cell b<tr><td>cell c<td>cell d</table>
<div><span>unclosed span <a href="/index.html">link inside</div>
<p>Text after the mess & stray ampersand < not a tag > done.
<blockquote>quote never closed
""")

w("plain.txt", "Plain text document\n====================\n\nThis is served as text/plain and must render in a monospace font\nwith preserved line breaks.\n\n    indented block\n\ttab-indented line\n\nThe end.\n")
open(os.path.join(out, "download.bin"), "wb").write(bytes(range(256)) * 24)   # 6144 bytes -> truncated to 4 KB in ramfs
print("site generated in", out, sorted(os.listdir(out)))
