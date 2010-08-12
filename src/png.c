/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

void
image_png_read_header(image *im, const char *file)
{
  if ( (im->stdio_fp = fopen(file, "rb")) == NULL ) {
		croak("Image::Scale could not open %s for reading", file);
	}
	
  im->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if ( !im->png_ptr )
    croak("Image::Scale could not initialize libpng");
  
  im->info_ptr = png_create_info_struct(im->png_ptr);
  if ( !im->info_ptr ) {
    png_destroy_read_struct(&im->png_ptr, (png_infopp)NULL, (png_infopp)NULL);
    croak("Image::Scale could not initialize libpng");
  }
  
  if ( setjmp( png_jmpbuf(im->png_ptr) ) ) {
    return;
  }
  
  png_init_io(im->png_ptr, im->stdio_fp);
  
  png_read_info(im->png_ptr, im->info_ptr);
  
  im->width     = png_get_image_width(im->png_ptr, im->info_ptr);
  im->height    = png_get_image_height(im->png_ptr, im->info_ptr);
  im->channels  = png_get_channels(im->png_ptr, im->info_ptr);
  im->has_alpha = 1;
}

void
hexdump(unsigned char *data, uint32_t size)
{
  unsigned char c;
  int i = 1;
  int n;
  char bytestr[4] = {0};
  char hexstr[ 16*3 + 5] = {0};
  char charstr[16*1 + 5] = {0};
  
  if (!size) {
    return;
  }
  
  for (n = 0; n < size; n++) {
    c = data[n];

    /* store hex str (for left side) */
    snprintf(bytestr, sizeof(bytestr), "%02x ", c);
    strncat(hexstr, bytestr, sizeof(hexstr)-strlen(hexstr)-1);

    /* store char str (for right side) */
    if (c < 21 || c > 0x7E) {
      c = '.';
    }
    snprintf(bytestr, sizeof(bytestr), "%c", c);
    strncat(charstr, bytestr, sizeof(charstr)-strlen(charstr)-1);

    if (i % 16 == 0) { 
      /* line completed */
      PerlIO_printf(PerlIO_stderr(), "%-50.50s  %s\n", hexstr, charstr);
      hexstr[0] = 0;
      charstr[0] = 0;
    }
    i++;
  }

  if (strlen(hexstr) > 0) {
    /* print rest of buffer if not empty */
    PerlIO_printf(PerlIO_stderr(), "%-50.50s  %s\n", hexstr, charstr);
  }
}

static void
image_png_interlace_pass(image *im, unsigned char *ptr, int start_y, int stride_y, int start_x, int stride_x)
{
  int x, y;
  
  for (y = 0; y < im->height; y++) {
    png_read_row(im->png_ptr, ptr, NULL);
    if (start_y == 0) {
      start_y = stride_y;
      for (x = start_x; x < im->width; x += stride_x) {
        im->pixbuf[y * im->width + x] = COL_FULL(
          ptr[x * 4], ptr[x * 4 + 1], ptr[x * 4 + 2], ptr[x * 4 + 3]
        );
      }
    }
    start_y--;
  }
}

void
image_png_load(image *im)
{
  int bit_depth, color_type, num_passes, pass, x, y;
  int ofs;
  unsigned char *ptr;
  
  if ( setjmp( png_jmpbuf(im->png_ptr) ) ) {
    return;
  }
  
  bit_depth  = png_get_bit_depth(im->png_ptr, im->info_ptr);
  color_type = png_get_color_type(im->png_ptr, im->info_ptr);
  
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(im->png_ptr);
  
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(im->png_ptr);
  
  if (png_get_valid(im->png_ptr, im->info_ptr, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(im->png_ptr);
  
  if (bit_depth == 16)
    png_set_strip_16(im->png_ptr);
  else if (bit_depth < 8)
    png_set_packing(im->png_ptr);
  
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY) {
    png_set_add_alpha(im->png_ptr, 0xff, PNG_FILLER_AFTER);
  }
    
  num_passes = png_set_interlace_handling(im->png_ptr);
  
  DEBUG_TRACE("png bit_depth %d, color_type %d, channels %d, num_passes %d\n", bit_depth, color_type, im->channels, num_passes);
  
  png_read_update_info(im->png_ptr, im->info_ptr);
  
  image_alloc(im, im->width, im->height);
  
  New(0, ptr, png_get_rowbytes(im->png_ptr, im->info_ptr), unsigned char);
  
  // XXX interlaced could be optimized better, as pixbuf operates on pixels that did not change
  
  if (num_passes == 1) {
    // Non-interlaced
    ofs = 0;
    for (y = 0; y < im->height; y++) {
      png_read_row(im->png_ptr, ptr, NULL);    
      for (x = 0; x < im->width; x++) {
			  im->pixbuf[ofs++] = COL_FULL(ptr[x * 4], ptr[x * 4 + 1], ptr[x * 4 + 2], ptr[x * 4 + 3]);
			}
    }
  }
  else if (num_passes == 7) {
    // Interlaced
      
    // The first pass will return an image 1/8 as wide as the entire image
    // (every 8th column starting in column 0)
    // and 1/8 as high as the original (every 8th row starting in row 0)
    image_png_interlace_pass(im, ptr, 0, 8, 0, 8);
    
    // The second will be 1/8 as wide (starting in column 4)
    // and 1/8 as high (also starting in row 0)
    image_png_interlace_pass(im, ptr, 0, 8, 4, 8);
    
    // The third pass will be 1/4 as wide (every 4th pixel starting in column 0)
    // and 1/8 as high (every 8th row starting in row 4)
    image_png_interlace_pass(im, ptr, 4, 8, 0, 4);
    
    // The fourth pass will be 1/4 as wide and 1/4 as high
    // (every 4th column starting in column 2, and every 4th row starting in row 0)
    image_png_interlace_pass(im, ptr, 0, 4, 2, 4);
    
    // The fifth pass will return an image 1/2 as wide,
    // and 1/4 as high (starting at column 0 and row 2)
    image_png_interlace_pass(im, ptr, 2, 4, 0, 2);
    
    // The sixth pass will be 1/2 as wide and 1/2 as high as the original
    // (starting in column 1 and row 0)
    image_png_interlace_pass(im, ptr, 0, 2, 1, 2);
    
    // The seventh pass will be as wide as the original, and 1/2 as high,
    // containing all of the odd numbered scanlines.
    image_png_interlace_pass(im, ptr, 1, 2, 0, 1);
  }
  else {
    croak("Image::Scale unsupported PNG interlace type (%d passes)\n", num_passes);
  }
  
  Safefree(ptr);
  
  png_read_end(im->png_ptr, im->info_ptr);
}

void
image_png_save(image *im, const char *path)
{
  png_structp png_ptr;
  png_infop info_ptr;
  FILE *out;
  int color_space, i, x, y;
  unsigned char *ptr;
  
  if ((out = fopen(path, "wb")) == NULL) {
    croak("Image::Scale cannot open %s for writing", path);
  }
  
  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr) {
    fclose(out);
    croak("Image::Scale could not initialize libpng");
  }
  
  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, NULL);
    fclose(out);
    croak("Image::Scale could not initialize libpng");
  }
  
  if (setjmp( png_jmpbuf(png_ptr) )) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(out);
    return;
  }
  
  png_init_io(png_ptr, out);
  
  /* XXX
  switch (im->channels) {
    case 4: color_space = PNG_COLOR_TYPE_RGB_ALPHA;  break;
    case 3: color_space = PNG_COLOR_TYPE_RGB;        break;
    case 2: color_space = PNG_COLOR_TYPE_GRAY_ALPHA; break;
    case 1: color_space = PNG_COLOR_TYPE_GRAY;       break;
  }
  */
  
  png_set_IHDR(png_ptr, info_ptr, im->target_width, im->target_height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  
  png_write_info(png_ptr, info_ptr);
  
  New(0, ptr, png_get_rowbytes(png_ptr, info_ptr), unsigned char);
  
  i = 0;
  for (y = 0; y < im->target_height; y++) {
    for (x = 0; x < im->target_width; x++)	{
			ptr[x * 4]     = COL_RED(im->outbuf[i]);
			ptr[x * 4 + 1] = COL_GREEN(im->outbuf[i]);
			ptr[x * 4 + 2] = COL_BLUE(im->outbuf[i]);
      ptr[x * 4 + 3] = COL_ALPHA(im->outbuf[i]);
			i++;
		}
		png_write_row(png_ptr, (png_bytep)ptr);
	}
	
  Safefree(ptr);
  
  png_write_end(png_ptr, info_ptr);
  
  png_destroy_write_struct(&png_ptr, &info_ptr);
  
  fclose(out);
}

void
image_png_finish(image *im)
{
  if (im->png_ptr != NULL) {
    png_destroy_read_struct(&im->png_ptr, &im->info_ptr, NULL);
    im->png_ptr = NULL;
  
    fclose(im->stdio_fp);
  }
}
