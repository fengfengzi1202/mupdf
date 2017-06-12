#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

#include "../fitz/colorspace-imp.h"

#include <string.h>

/* ICCBased */
static fz_colorspace *
load_icc_based(fz_context *ctx, pdf_obj *dict, int alt)
{
	int n;
	pdf_obj *obj;
	fz_buffer *buffer = NULL;
	fz_colorspace *cs = NULL;

	n = pdf_to_int(ctx, pdf_dict_get(ctx, dict, PDF_NAME_N));

	fz_try(ctx)
	{
		if (fz_icc_engine(ctx))
		{
			buffer = pdf_load_stream(ctx, dict);
			cs = fz_new_icc_colorspace(ctx, 0, n, buffer, NULL);
		}

		/* Use alternate if ICC not invalid */
		if (alt)
		{
			if (cs == NULL)
			{
				obj = pdf_dict_get(ctx, dict, PDF_NAME_Alternate);
				if (obj)
				{
					cs = pdf_load_colorspace(ctx, obj);
					if (cs->n != n)
					{
						fz_drop_colorspace(ctx, cs);
						fz_throw(ctx, FZ_ERROR_GENERIC, "ICCBased /Alternate colorspace must have %d components", n);
					}
				}
				else
				{
					switch (n)
					{
					case 1:
						cs = fz_device_gray(ctx);
						break;
					case 3:
						cs = fz_device_rgb(ctx);
						break;
					case 4:
						cs = fz_device_cmyk(ctx);
						break;
					default: fz_throw(ctx, FZ_ERROR_SYNTAX, "ICCBased must have 1, 3 or 4 components");
					}
				}
			}
			else
			{
				/* We need to check if the alternate color space is CIELAB which
				 * would let us know that we need to apply CIELAB clamping */
				fz_colorspace *cs_alt = NULL;

				obj = pdf_dict_get(ctx, dict, PDF_NAME_Alternate);
				if (obj)
				{
					cs_alt = pdf_load_colorspace(ctx, obj);
					if (fz_colorspace_is_icc(cs_alt) && cs_alt == fz_device_lab(ctx))
						cs->clamp = cs_alt->clamp;
					fz_drop_colorspace(ctx, cs_alt);
				}
			}
		}
		else
		{
			/* We need to check if the alternate color space is CIELAB which
			 * would let us know that we need to apply CIELAB clamping */
			obj = pdf_dict_get(ctx, dict, PDF_NAME_Alternate);
			if (obj)
			{
				fz_colorspace *cs_alt = pdf_load_colorspace(ctx, obj);

				if (cs_alt == fz_device_lab(ctx))
					cs->clamp = cs_alt->clamp;
				fz_drop_colorspace(ctx, cs_alt);
			}
		}
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, buffer);
	}
	fz_catch(ctx)
	{
		/* Something went wrong (perhaps invalid ICC profile) */
	}

	if (n == 1 || n == 3 || n == 4)
		return cs;
	fz_throw(ctx, FZ_ERROR_SYNTAX, "ICCBased must have 1, 3 or 4 components");
}

struct separation
{
	fz_colorspace *base;
	pdf_function *tint;
};

static void
separation_to_alt(fz_context *ctx, fz_colorspace *cs, const float *color, float *alt)
{
	struct separation *sep = cs->data;
	pdf_eval_function(ctx, sep->tint, color, cs->n, alt, sep->base->n);
}

static void
separation_to_rgb(fz_context *ctx, fz_colorspace *cs, const float *color, float *rgb)
{
	struct separation *sep = cs->data;
	float alt[FZ_MAX_COLORS];
	pdf_eval_function(ctx, sep->tint, color, cs->n, alt, sep->base->n);
	fz_convert_color(ctx, fz_cs_params(ctx), NULL, fz_device_rgb(ctx), rgb, sep->base, alt);
}

static void
free_separation(fz_context *ctx, fz_colorspace *cs)
{
	struct separation *sep = cs->data;
	fz_drop_colorspace(ctx, sep->base);
	pdf_drop_function(ctx, sep->tint);
	fz_free(ctx, sep);
}

static fz_colorspace *
base_separation(const fz_colorspace *cs)
{
	struct separation *sep = cs->data;

	return sep->base;
}

static fz_colorspace *
load_separation(fz_context *ctx, pdf_obj *array)
{
	fz_colorspace *cs;
	struct separation *sep = NULL;
	pdf_obj *nameobj = pdf_array_get(ctx, array, 1);
	pdf_obj *baseobj = pdf_array_get(ctx, array, 2);
	pdf_obj *tintobj = pdf_array_get(ctx, array, 3);
	fz_colorspace *base;
	pdf_function *tint = NULL;
	int n;

	fz_var(tint);
	fz_var(sep);

	if (pdf_is_array(ctx, nameobj))
		n = pdf_array_len(ctx, nameobj);
	else
		n = 1;

	if (n > FZ_MAX_COLORS)
		fz_throw(ctx, FZ_ERROR_SYNTAX, "too many components in colorspace");

	base = pdf_load_colorspace(ctx, baseobj);

	fz_try(ctx)
	{
		tint = pdf_load_function(ctx, tintobj, n, base->n);
		/* RJW: fz_drop_colorspace(ctx, base);
		 * "cannot load tint function (%d 0 R)", pdf_to_num(ctx, tintobj) */

		sep = fz_malloc_struct(ctx, struct separation);
		sep->base = fz_keep_colorspace(ctx, base);  /* We drop it during the sep free... */
		sep->tint = tint;

		cs = fz_new_colorspace(ctx, n == 1 ? "Separation" : "DeviceN", 0, n, 1,
			fz_colorspace_is_icc(fz_device_rgb(ctx)) ? separation_to_alt : separation_to_rgb, NULL, base_separation, NULL, free_separation, sep,
			sizeof(struct separation) + (base ? base->size : 0) + pdf_function_size(ctx, tint));
	}
	fz_catch(ctx)
	{
		fz_drop_colorspace(ctx, base);
		pdf_drop_function(ctx, tint);
		fz_free(ctx, sep);
		fz_rethrow(ctx);
	}

	return cs;
}

int
pdf_is_tint_colorspace(fz_context *ctx, fz_colorspace *cs)
{
	return cs && cs->free_data == free_separation;
}

/* Indexed */

static fz_colorspace *
load_indexed(fz_context *ctx, pdf_obj *array)
{
	pdf_obj *baseobj = pdf_array_get(ctx, array, 1);
	pdf_obj *highobj = pdf_array_get(ctx, array, 2);
	pdf_obj *lookupobj = pdf_array_get(ctx, array, 3);
	fz_colorspace *base = NULL;
	fz_colorspace *cs;
	int i, n, high;
	unsigned char *lookup = NULL;

	fz_var(base);
	fz_var(lookup);

	fz_try(ctx)
	{
		base = pdf_load_colorspace(ctx, baseobj);

		high = pdf_to_int(ctx, highobj);
		high = fz_clampi(high, 0, 255);
		n = base->n * (high + 1);
		lookup = fz_malloc_array(ctx, 1, n);

		if (pdf_is_string(ctx, lookupobj) && pdf_to_str_len(ctx, lookupobj) >= n)
		{
			unsigned char *buf = (unsigned char *) pdf_to_str_buf(ctx, lookupobj);
			for (i = 0; i < n; i++)
				lookup[i] = buf[i];
		}
		else if (pdf_is_indirect(ctx, lookupobj))
		{
			fz_stream *file = NULL;

			fz_var(file);

			fz_try(ctx)
			{
				file = pdf_open_stream(ctx, lookupobj);
				i = (int)fz_read(ctx, file, lookup, n);
				if (i < n)
					memset(lookup+i, 0, n-i);
			}
			fz_always(ctx)
			{
				fz_drop_stream(ctx, file);
			}
			fz_catch(ctx)
			{
				fz_rethrow(ctx);
			}
		}
		else
		{
			fz_throw(ctx, FZ_ERROR_SYNTAX, "cannot parse colorspace lookup table");
		}

		cs = fz_new_indexed_colorspace(ctx, base, high, lookup);
	}
	fz_catch(ctx)
	{
		fz_drop_colorspace(ctx, base);
		fz_free(ctx, lookup);
		fz_rethrow(ctx);
	}

	return cs;
}

/* If an issue returns -1, else returns 0 */
static int
pdf_cal_common(fz_context *ctx, pdf_obj *dict, float *wp, float *bp, float *gamma)
{
	pdf_obj *obj;
	pdf_obj *objv;
	int i;

	obj = pdf_dict_get(ctx, dict, PDF_NAME_WhitePoint);
	if (pdf_is_array(ctx, obj) && pdf_array_len(ctx, obj) == 3)
	{
		for (i = 0; i < 3; i++)
		{
			objv = pdf_array_get(ctx, obj, i);
			if (!pdf_is_number(ctx, objv))
				return -1;
			wp[i] = pdf_to_real(ctx, objv);
		}
		if (wp[1] != 1)
			return -1;
	}
	else
		return -1;

	obj = pdf_dict_get(ctx, dict, PDF_NAME_BlackPoint);
	if (pdf_is_array(ctx, obj) && pdf_array_len(ctx, obj) == 3)
	{
		for (i = 0; i < 3; i++)
		{
			objv = pdf_array_get(ctx, obj, i);
			if (!pdf_is_number(ctx, objv))
			{
				return -1;
			}
			bp[i] = pdf_to_real(ctx, objv);
			fz_clamp(bp[i], 0, 1);
		}
	}

	obj = pdf_dict_get(ctx, dict, PDF_NAME_Gamma);
	if (pdf_is_number(ctx, obj))
	{
		*gamma = pdf_to_real(ctx, objv);
		if (*gamma < 0 || *gamma == 0)
			return -1;
	}
	else if (pdf_is_array(ctx, obj) && pdf_array_len(ctx, obj) == 3)
	{
		for (i = 0; i < 3; i++)
		{
			objv = pdf_array_get(ctx, obj, i);
			if (!pdf_is_number(ctx, objv))
				return -1;
			gamma[i] = pdf_to_real(ctx, objv);
		}
	}
	return 0;
}

static fz_colorspace *
pdf_calgray(fz_context *ctx, pdf_obj *dict)
{
	float wp[3];
	float bp[3] = { 0 };
	float gamma = 1.0;

	if (dict == NULL)
		return fz_device_gray(ctx);

	if (pdf_cal_common(ctx, dict, wp, bp, &gamma) == 0)
		return fz_new_cal_colorspace(ctx, wp, bp, &gamma, NULL);

	return fz_device_gray(ctx);
}

static fz_colorspace *
pdf_calrgb(fz_context *ctx, pdf_obj *dict)
{
	pdf_obj *obj, *objv;
	float matrix[9] = { 1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0 };
	float wp[3];
	float bp[3] = { 0 };
	float gamma[3] = { 1.0, 1.0, 1.0 };
	int i;

	if (dict == NULL)
		return fz_device_rgb(ctx);

	if (pdf_cal_common(ctx, dict, wp, bp, gamma) == 0)
	{
		obj = pdf_dict_get(ctx, dict, PDF_NAME_Matrix);
		if (obj && pdf_is_array(ctx, obj) && pdf_array_len(ctx, obj) == 9)
		{
			for (i = 0; i < 9; i++)
			{
				objv = pdf_array_get(ctx, obj, i);
				if (!pdf_is_number(ctx, objv))
					return fz_device_rgb(ctx);
				matrix[i] = pdf_to_real(ctx, objv);
			}
			return fz_new_cal_colorspace(ctx, wp, bp, gamma, matrix);
		}
	}
	return fz_device_rgb(ctx);
}

/* Parse and create colorspace from PDF object */

static fz_colorspace *
pdf_load_colorspace_imp(fz_context *ctx, pdf_obj *obj)
{
	if (pdf_obj_marked(ctx, obj))
		fz_throw(ctx, FZ_ERROR_SYNTAX, "recursion in colorspace definition");

	if (pdf_is_name(ctx, obj))
	{
		if (pdf_name_eq(ctx, obj, PDF_NAME_Pattern))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_G))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_RGB))
			return fz_device_rgb(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_CMYK))
			return fz_device_cmyk(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceGray))
			return fz_device_gray(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceRGB))
			return fz_device_rgb(ctx);
		else if (pdf_name_eq(ctx, obj, PDF_NAME_DeviceCMYK))
			return fz_device_cmyk(ctx);
		else
			fz_throw(ctx, FZ_ERROR_SYNTAX, "unknown colorspace: %s", pdf_to_name(ctx, obj));
	}

	else if (pdf_is_array(ctx, obj))
	{
		pdf_obj *name = pdf_array_get(ctx, obj, 0);

		if (pdf_is_name(ctx, name))
		{
			/* load base colorspace instead */
			if (pdf_name_eq(ctx, name, PDF_NAME_G))
				return fz_device_gray(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_RGB))
				return fz_device_rgb(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceGray))
				return fz_device_gray(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceRGB))
				return fz_device_rgb(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceCMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalGray))
			{
				if (fz_icc_engine(ctx))
					return pdf_calgray(ctx, pdf_array_get(ctx, obj, 1));
				else
					return fz_device_gray(ctx);
			}
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalRGB))
			{
				if (fz_icc_engine(ctx))
					return pdf_calrgb(ctx, pdf_array_get(ctx, obj, 1));
				else
					return fz_device_rgb(ctx);
			}
			else if (pdf_name_eq(ctx, name, PDF_NAME_CalCMYK))
				return fz_device_cmyk(ctx);
			else if (pdf_name_eq(ctx, name, PDF_NAME_Lab))
				return fz_device_lab(ctx);
			else
			{
				fz_colorspace *cs;
				fz_try(ctx)
				{
					if (pdf_mark_obj(ctx, obj))
						fz_throw(ctx, FZ_ERROR_SYNTAX, "recursive colorspace");
					if (pdf_name_eq(ctx, name, PDF_NAME_ICCBased))
						cs = load_icc_based(ctx, pdf_array_get(ctx, obj, 1), 1);

					else if (pdf_name_eq(ctx, name, PDF_NAME_Indexed))
						cs = load_indexed(ctx, obj);
					else if (pdf_name_eq(ctx, name, PDF_NAME_I))
						cs = load_indexed(ctx, obj);

					else if (pdf_name_eq(ctx, name, PDF_NAME_Separation))
						cs = load_separation(ctx, obj);

					else if (pdf_name_eq(ctx, name, PDF_NAME_DeviceN))
						cs = load_separation(ctx, obj);
					else if (pdf_name_eq(ctx, name, PDF_NAME_Pattern))
					{
						pdf_obj *pobj;

						pobj = pdf_array_get(ctx, obj, 1);
						if (!pobj)
						{
							cs = fz_device_gray(ctx);
							break;
						}

						cs = pdf_load_colorspace(ctx, pobj);
					}
					else
						fz_throw(ctx, FZ_ERROR_SYNTAX, "unknown colorspace %s", pdf_to_name(ctx, name));
				}
				fz_always(ctx)
				{
					pdf_unmark_obj(ctx, obj);
				}
				fz_catch(ctx)
				{
					fz_rethrow(ctx);
				}
				return cs;
			}
		}
	}

	/* We have seen files where /DefaultRGB is specified as 1 0 R,
	 * and 1 0 obj << /Length 3144 /Alternate /DeviceRGB /N 3 >>
	 * stream ...iccprofile... endstream endobj.
	 * This *should* be [ /ICCBased 1 0 R ], but Acrobat seems to
	 * handle it, so do our best. */
	else if (pdf_is_dict(ctx, obj))
	{
		return load_icc_based(ctx, obj, 1);
	}

	fz_throw(ctx, FZ_ERROR_SYNTAX, "could not parse color space (%d 0 R)", pdf_to_num(ctx, obj));
}

fz_colorspace *
pdf_load_colorspace(fz_context *ctx, pdf_obj *obj)
{
	fz_colorspace *cs;

	if ((cs = pdf_find_item(ctx, fz_drop_colorspace_imp, obj)) != NULL)
	{
		return cs;
	}

	cs = pdf_load_colorspace_imp(ctx, obj);

	pdf_store_item(ctx, obj, cs, cs->size);

	return cs;
}

fz_colorspace *
pdf_get_oi(fz_context *ctx, pdf_document *doc)
{
	return doc->oi;
}

fz_colorspace *
pdf_load_oi(fz_context *ctx, pdf_document *doc)
{
	pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME_Root);
	pdf_obj *intents = pdf_dict_get(ctx, root, PDF_NAME_OutputIntents);
	pdf_obj *intent_dict;
	pdf_obj *dest_profile;

	/* An array of intents */
	if (!intents)
		return NULL;

	/* For now, always just use the first intent. I have never even seen a file
	 * with multiple intents but it could happen */
	intent_dict = pdf_array_get(ctx, intents, 0);
	if (!intent_dict)
		return NULL;
	dest_profile = pdf_dict_get(ctx, intent_dict, PDF_NAME_DestOutputProfile);
	if (!dest_profile)
		return NULL;

	return load_icc_based(ctx, dest_profile, 0);
}
