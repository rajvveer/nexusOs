"""A small, valid one-page PDF (PDF 1.4, Helvetica text and a filled rectangle) for the SumatraPDF check."""


def make_pdf(text='NexusOS Wine PDF test', color=(0.1, 0.4, 0.9)):
    content = ('%.2f %.2f %.2f rg 72 600 300 120 re f\nBT /F1 28 Tf 72 540 Td (%s) Tj ET\n' % (color + (text,))).encode('latin-1')
    objects = [
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
        b'<< /Length %d >>\nstream\n' % len(content) + content + b'endstream',
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
    ]
    out = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n'); offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out)); out += b'%d 0 obj\n' % number + body + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1)
    for offset in offsets: out += b'%010d 00000 n \n' % offset
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)
    return bytes(out)


if __name__ == '__main__':
    import sys
    open(sys.argv[1], 'wb').write(make_pdf())
