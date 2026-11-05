"""Contact sheet of ingested video frames.  python vsheet.py <take folder> <first> <last> <step> [cols] [width]"""
import sys, os
from PIL import Image, ImageDraw
B = r'C:\Users\dharm\AppData\Local\CaptureManager\Media\FootballArena\Mono Video Ingest'
take, a, b, st = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
cols = int(sys.argv[5]) if len(sys.argv) > 5 else 6
W = int(sys.argv[6]) if len(sys.argv) > 6 else 240
d = os.path.join(B, take, 'Video', 'video')
idx = list(range(a, b + 1, st))
ims = []
for i in idx:
    p = os.path.join(d, 'frame_%06d.jpg' % i)
    if not os.path.exists(p):
        continue
    im = Image.open(p); im = im.resize((W, int(im.height * W / im.width)))
    ImageDraw.Draw(im).text((4, 4), str(i), fill=(255, 0, 0))
    ims.append(im)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * W, rows * ims[0].height), 'white')
for k, im in enumerate(ims):
    sheet.paste(im, ((k % cols) * W, (k // cols) * im.height))
from mocap_lib import ROOT
out = os.path.join(ROOT, 'report', 'video_%s_%d_%d.jpg' % (take, a, b))
sheet.save(out, quality=80)
print(out, sheet.size, Image.open(os.path.join(d, 'frame_%06d.jpg' % a)).size)
