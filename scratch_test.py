import os
from PIL import ImageFont

font_path = "E:\\AI\\Longboard-Display\\BebasNeue.ttf"
font = ImageFont.truetype(font_path, 24)

for char in [' ', 'A', 'g', '1']:
    mask = font.getmask(char, mode='L')
    bbox = font.getbbox(char)
    print(f"Char: {repr(char)}, mask size: {mask.size}, bbox: {bbox}")
