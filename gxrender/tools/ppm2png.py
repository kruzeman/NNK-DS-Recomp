import sys
try:
    from PIL import Image
except ImportError:
    sys.exit("no PIL")
im = Image.open(sys.argv[1])
im.save(sys.argv[2])
print(im.size)
