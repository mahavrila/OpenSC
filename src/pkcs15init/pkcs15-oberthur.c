/*
 * Oberthur specific operation for PKCS #15 initialization
 *
 * Copyright (C) 2002  Juha Yrj�l� <juha.yrjola@iki.fi>
 * Copyright (C) 2009  Viktor Tarasov <viktor.tarasov@opentrust.com>,
 *                     OpenTrust <www.opentrust.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>

#ifdef ENABLE_OPENSSL
#include <openssl/sha.h>
#endif

#include <opensc/opensc.h>
#include <opensc/cardctl.h>
#include <opensc/log.h>
#include "pkcs15-init.h"
#include "profile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COSM_TITLE "OberthurAWP"

#define TLV_TYPE_V	0
#define TLV_TYPE_LV	1
#define TLV_TYPE_TLV	2

/* Should be greater then SC_PKCS15_TYPE_CLASS_MASK */
#define SC_DEVICE_SPECIFIC_TYPE	 0x1000

#define COSM_TYPE_PRKEY_RSA (SC_DEVICE_SPECIFIC_TYPE | SC_PKCS15_TYPE_PRKEY_RSA)
#define COSM_TYPE_PUBKEY_RSA (SC_DEVICE_SPECIFIC_TYPE | SC_PKCS15_TYPE_PUBKEY_RSA)

#define COSM_TOKEN_FLAG_PRN_GENERATION		0x01
#define COSM_TOKEN_FLAG_LOGIN_REQUIRED		0x04
#define COSM_TOKEN_FLAG_USER_PIN_INITIALIZED	0x08
#define COSM_TOKEN_FLAG_TOKEN_INITIALIZED	0x0400

static int cosm_create_reference_data(struct sc_profile *, struct sc_pkcs15_card *,
		struct sc_pkcs15_pin_info *, const unsigned char *, size_t,
		const unsigned char *, size_t);
static int cosm_update_pin(struct sc_profile *, struct sc_pkcs15_card *,
		struct sc_pkcs15_pin_info *, const unsigned char *, size_t,
		const unsigned char *, size_t);
int cosm_delete_file(struct sc_pkcs15_card *, struct sc_profile *, struct sc_file *);

static int 
cosm_write_tokeninfo (struct sc_pkcs15_card *p15card, struct sc_profile *profile, 
		char *label, unsigned p15_flags)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *file = NULL;
	unsigned mask = SC_PKCS15_CARD_FLAG_PRN_GENERATION 
		| SC_PKCS15_CARD_FLAG_LOGIN_REQUIRED 
		| SC_PKCS15_CARD_FLAG_USER_PIN_INITIALIZED 
		| SC_PKCS15_CARD_FLAG_TOKEN_INITIALIZED;
	int rv, sz, flags = 0;
	char *buffer = NULL;

	if (!p15card || !profile)
		return SC_ERROR_INVALID_ARGUMENTS;
	
	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "cosm_write_tokeninfo() label '%s'; flags 0x%X\n", label, p15_flags);
	if (sc_profile_get_file(profile, COSM_TITLE"-token-info", &file))
		SC_TEST_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "Cannot find "COSM_TITLE"-token-info");

	if (file->size < 16)
		SC_TEST_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "Unsufficient size of the "COSM_TITLE"-token-info file");
	
	buffer = calloc(1, file->size);
	if (!buffer)
		SC_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "Allocation error in cosm_write_tokeninfo()");

	if (label)   
		strncpy(buffer, label, file->size - 4);
	else if (p15card->label)
		snprintf(buffer, file->size - 4, p15card->label);
	else if (profile->p15_spec && profile->p15_spec->label)
		snprintf(buffer, file->size - 4, profile->p15_spec->label);
	else
		snprintf(buffer, file->size - 4, "OpenSC-Token");

	sz = strlen(buffer);	
	if (sz < file->size - 4)
		memset(buffer + sz, ' ', file->size - sz);

	if (p15_flags & SC_PKCS15_CARD_FLAG_PRN_GENERATION)
		flags |= COSM_TOKEN_FLAG_PRN_GENERATION;
	
	if (p15_flags & SC_PKCS15_CARD_FLAG_LOGIN_REQUIRED)
		flags |= COSM_TOKEN_FLAG_LOGIN_REQUIRED;
	
	if (p15_flags & SC_PKCS15_CARD_FLAG_USER_PIN_INITIALIZED)
		flags |= COSM_TOKEN_FLAG_USER_PIN_INITIALIZED;
	
	if (p15_flags & SC_PKCS15_CARD_FLAG_TOKEN_INITIALIZED)
		flags |= COSM_TOKEN_FLAG_TOKEN_INITIALIZED;

	sc_debug(ctx, "cosm_write_tokeninfo() token label '%s'; oberthur flags 0x%X\n", buffer, flags);

	memset(buffer + file->size - 4, 0, 4);
	*(buffer + file->size - 1) = flags & 0xFF;
	*(buffer + file->size - 2) = (flags >> 8) & 0xFF;

	rv = sc_pkcs15init_update_file(profile, p15card, file, buffer, file->size);
	if (rv > 0)
		rv = 0;

	p15card->flags = (p15card->flags & ~mask) | p15_flags;

	if (profile->p15_spec)
		profile->p15_spec->flags = (profile->p15_spec->flags & ~mask) | p15_flags;

	free(buffer);
	SC_FUNC_RETURN(ctx, 1, rv);
}


int 
cosm_delete_file(struct sc_pkcs15_card *p15card, struct sc_profile *profile,
		struct sc_file *df)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_path  path;
	struct sc_file  *parent;
	int rv = 0;

	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "id %04X\n", df->id);
	if (df->type==SC_FILE_TYPE_DF)   {
		rv = sc_pkcs15init_authenticate(profile, p15card, df, SC_AC_OP_DELETE);
		SC_TEST_RET(ctx, rv, "Cannot authenticate SC_AC_OP_DELETE");
	}
	
	/* Select the parent DF */
	path = df->path;
	path.len -= 2;

	rv = sc_select_file(p15card->card, &path, &parent);
	SC_TEST_RET(ctx, rv, "Cannnot select parent");

	rv = sc_pkcs15init_authenticate(profile, p15card, parent, SC_AC_OP_DELETE);
	sc_file_free(parent);
	SC_TEST_RET(ctx, rv, "Cannnot authenticate SC_AC_OP_DELETE");

	memset(&path, 0, sizeof(path));
	path.type = SC_PATH_TYPE_FILE_ID;
	path.value[0] = df->id >> 8;
	path.value[1] = df->id & 0xFF;
	path.len = 2;

	rv = sc_delete_file(p15card->card, &path);

	SC_FUNC_RETURN(ctx, 1, rv);
}


/*
 * Erase the card
 */
static int 
cosm_erase_card(struct sc_profile *profile, struct sc_pkcs15_card *p15card)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file  *df = profile->df_info->file, *dir;
	int rv;

	SC_FUNC_CALLED(ctx, 1);
	/* Delete EF(DIR). This may not be very nice
	 * against other applications that use this file, but
	 * extremely useful for testing :)
	 * Note we need to delete if before the DF because we create
	 * it *after* the DF. 
	 * */
	if (sc_profile_get_file(profile, "DIR", &dir) >= 0) {
		sc_debug(ctx, "erase file dir %04X\n",dir->id);
		rv = cosm_delete_file(p15card, profile, dir);
		sc_file_free(dir);
		if (rv < 0 && rv != SC_ERROR_FILE_NOT_FOUND)
			goto done;
	}

	sc_debug(ctx, "erase file ddf %04X\n",df->id);
	rv = cosm_delete_file(p15card, profile, df);

	if (sc_profile_get_file(profile, "private-DF", &dir) >= 0) {
		sc_debug(ctx, "erase file dir %04X\n",dir->id);
		rv = cosm_delete_file(p15card, profile, dir);
		sc_file_free(dir);
		if (rv < 0 && rv != SC_ERROR_FILE_NOT_FOUND)
			goto done;
	}
	
	if (sc_profile_get_file(profile, "public-DF", &dir) >= 0) {
		sc_debug(ctx, "erase file dir %04X\n",dir->id);
		rv = cosm_delete_file(p15card, profile, dir);
		sc_file_free(dir);
		if (rv < 0 && rv != SC_ERROR_FILE_NOT_FOUND)
			goto done;
	}

	rv = sc_profile_get_file(profile, COSM_TITLE"-AppDF", &dir);
	if (!rv) {
		sc_debug(ctx, "delete %s; r %i\n", COSM_TITLE"-AppDF", rv);
		rv = cosm_delete_file(p15card, profile, dir);
		sc_file_free(dir);
	}

	sc_free_apps(p15card->card);
done:		
	if (rv == SC_ERROR_FILE_NOT_FOUND)
		rv = 0;

	SC_FUNC_RETURN(ctx, 1, rv);
}


static int
cosm_create_dir(struct sc_profile *profile, struct sc_pkcs15_card *p15card, 
		struct sc_file *df)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *file = NULL;
	size_t ii;
	int rv;
	static const char *create_dfs[] = {
		COSM_TITLE"-AppDF",
		"private-DF",
		"public-DF",
		COSM_TITLE"-token-info",
		COSM_TITLE"-puk-file",
		COSM_TITLE"-container-list",
		COSM_TITLE"-public-list",
		COSM_TITLE"-private-list",
#if 0
		"PKCS15-AppDF",
		"PKCS15-ODF",
		"PKCS15-AODF",
		"PKCS15-PrKDF",
		"PKCS15-PuKDF",
		"PKCS15-CDF",
		"PKCS15-DODF",
#endif
		NULL
	};

	SC_FUNC_CALLED(ctx, 1);
		        
	rv = sc_pkcs15init_create_file(profile, p15card, df);
	SC_TEST_RET(ctx, rv, "Failed to create DIR DF");

	/* Oberthur AWP file system is expected.*/
	/* Create private objects DF */
	for (ii = 0; create_dfs[ii]; ii++)   {
		if (sc_profile_get_file(profile, create_dfs[ii], &file))   {
			sc_debug(ctx, "Inconsistent profile: cannot find %s", create_dfs[ii]);
			SC_TEST_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "Profile do not contains Oberthur AWP file");
		}
	
		rv = sc_pkcs15init_create_file(profile, p15card, file);
		sc_file_free(file);
		if (rv != SC_ERROR_FILE_ALREADY_EXISTS)
			SC_TEST_RET(ctx, rv, "Failed to create Oberthur AWP file");
	}

	rv = cosm_write_tokeninfo(p15card, profile, NULL,
		SC_PKCS15_CARD_FLAG_TOKEN_INITIALIZED | SC_PKCS15_CARD_FLAG_PRN_GENERATION);

	SC_FUNC_RETURN(ctx, 1, rv);
}


static int 
cosm_create_reference_data(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_pin_info *pinfo, 
		const unsigned char *pin, size_t pin_len,	
		const unsigned char *puk, size_t puk_len )
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_card *card = p15card->card;
	struct sc_pkcs15_pin_info profile_pin;
	struct sc_pkcs15_pin_info profile_puk;
	struct sc_cardctl_oberthur_createpin_info args;
	unsigned char *puk_buff = NULL;
	int rv;
	unsigned char oberthur_puk[16] = {
		0x6F, 0x47, 0xD9, 0x88, 0x4B, 0x6F, 0x9D, 0xC5,
		0x78, 0x33, 0x79, 0x8F, 0x5B, 0x7D, 0xE1, 0xA5
	};

	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "pin lens %i/%i\n", pin_len,  puk_len);
	if (!pin || pin_len>0x40)
		return SC_ERROR_INVALID_ARGUMENTS;
	if (puk && !puk_len)
		return SC_ERROR_INVALID_ARGUMENTS;

	rv = sc_select_file(card, &pinfo->path, NULL);
	SC_TEST_RET(ctx, rv, "Cannot select file");

	sc_profile_get_pin_info(profile, SC_PKCS15INIT_USER_PIN, &profile_pin);
	sc_profile_get_pin_info(profile, SC_PKCS15INIT_USER_PUK, &profile_puk);

	memset(&args, 0, sizeof(args));
	args.type = SC_AC_CHV;
	args.ref = pinfo->reference;
	args.pin = pin;
	args.pin_len = pin_len;

	if (!(pinfo->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN))   {
		args.pin_tries = profile_pin.tries_left;
		if (profile_puk.tries_left > 0)   {
			args.puk = oberthur_puk;
			args.puk_len = sizeof(oberthur_puk);
			args.puk_tries = 5;
		}
	}
	else   {
		args.pin_tries = profile_puk.tries_left;
	}

	rv = sc_card_ctl(card, SC_CARDCTL_OBERTHUR_CREATE_PIN, &args);
	SC_TEST_RET(ctx, rv, "'CREATE_PIN' card specific command failed");

	if (!(pinfo->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN) && (profile_puk.tries_left > 0))   {
	        struct sc_file *file = NULL;

		if (sc_profile_get_file(profile, COSM_TITLE"-puk-file", &file))
			SC_TEST_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "Cannot find PUKFILE");

		rv = sc_pkcs15init_update_file(profile, p15card, file, oberthur_puk, sizeof(oberthur_puk));
		SC_TEST_RET(ctx, rv, "Failed to update pukfile");

		if (file)
			sc_file_free(file);
	}

	if (puk_buff)
		free(puk_buff);
	
	SC_FUNC_RETURN(ctx, 1, rv);
}


/*
 * Update PIN
 */
static int 
cosm_update_pin(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_pin_info *pinfo, const unsigned char *pin, size_t pin_len,
		const unsigned char *puk, size_t puk_len )
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv;
	
	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "ref %i; flags 0x%X\n", pinfo->reference, pinfo->flags);

	if (pinfo->flags & SC_PKCS15_PIN_FLAG_SO_PIN)   {
		if (pinfo->reference != 4)
			SC_TEST_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "cosm_update_pin() invalid SOPIN reference");
		sc_debug(ctx, "Update SOPIN ignored\n");
		rv = SC_SUCCESS;
	}
	else   {
		rv = cosm_create_reference_data(profile, p15card, pinfo, 
				pin, pin_len, puk, puk_len);
		SC_TEST_RET(ctx, rv, "cosm_update_pin() failed to change PIN");

		rv = cosm_write_tokeninfo(p15card, profile, NULL,
			SC_PKCS15_CARD_FLAG_TOKEN_INITIALIZED 
			| SC_PKCS15_CARD_FLAG_PRN_GENERATION
			| SC_PKCS15_CARD_FLAG_LOGIN_REQUIRED
			| SC_PKCS15_CARD_FLAG_USER_PIN_INITIALIZED);
		SC_TEST_RET(ctx, rv, "cosm_update_pin() failed to update tokeninfo");
	}

	SC_FUNC_RETURN(ctx, 1, rv);
}


static int
cosm_select_pin_reference(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_pin_info *pin_info) 
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *pinfile;

	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "ref %i; flags %X\n", pin_info->reference, pin_info->flags);
	if (sc_profile_get_file(profile, COSM_TITLE "-AppDF", &pinfile) < 0) {
		sc_debug(ctx, "Profile doesn't define \"%s\"", COSM_TITLE "-AppDF");
		return SC_ERROR_INCONSISTENT_PROFILE;
	}

	if (pin_info->flags & SC_PKCS15_PIN_FLAG_LOCAL)
		pin_info->path = pinfile->path;

	sc_file_free(pinfile);
	
	if (pin_info->reference <= 0)   {
		if (pin_info->flags & SC_PKCS15_PIN_FLAG_SO_PIN)   
			pin_info->reference = 4;
		else if (pin_info->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN)
			pin_info->reference = 4;	
		else  
			pin_info->reference = 1;

		if (pin_info->flags & SC_PKCS15_PIN_FLAG_LOCAL)
			pin_info->reference |= 0x80;
	}

	SC_FUNC_RETURN(ctx, 1, SC_SUCCESS);
}


/*
 * Store a PIN
 */
static int
cosm_create_pin(struct sc_profile *profile, struct sc_pkcs15_card *p15card, 
		struct sc_file *df, struct sc_pkcs15_object *pin_obj,
		const unsigned char *pin, size_t pin_len,
		const unsigned char *puk, size_t puk_len)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_pin_info *pin_info = (struct sc_pkcs15_pin_info *) pin_obj->data;
	struct sc_file *pin_file;
	int rv = 0, type;

	SC_FUNC_CALLED(ctx, 1);
	sc_debug(ctx, "create '%s'; ref 0x%X; flags %X\n", pin_obj->label, pin_info->reference, pin_info->flags);
	if (sc_profile_get_file(profile, COSM_TITLE "-AppDF", &pin_file) < 0)
		SC_TEST_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "\""COSM_TITLE"-AppDF\" not defined");

	if (pin_info->flags & SC_PKCS15_PIN_FLAG_LOCAL)
		pin_info->path = pin_file->path;

	sc_file_free(pin_file);
	
	if (pin_info->flags & SC_PKCS15_PIN_FLAG_SO_PIN)   {
		if (pin_info->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN)   {
			SC_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "SOPIN unblocking is not supported");
		}
		else   {
			if (pin_info->reference != 4)  
				SC_TEST_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Invalid SOPIN reference");
			type = SC_PKCS15INIT_SO_PIN;
		}
	} 
	else {
		if (pin_info->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN)   {
			if (pin_info->reference != 0x84)  
				SC_TEST_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Invalid User PUK reference");
			type = SC_PKCS15INIT_USER_PUK;
		}
		else   {
			if (pin_info->reference != 0x81)
				SC_TEST_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Invalid User PIN reference");
			type = SC_PKCS15INIT_USER_PIN;
		}
	}

	if (pin && pin_len)   {
		rv = cosm_update_pin(profile, p15card, pin_info, pin, pin_len,  puk, puk_len);
		SC_TEST_RET(ctx, rv, "Update PIN failed");
	}

	SC_FUNC_RETURN(ctx, 1, rv);
}


/*
 * Allocate a file
 */
static int
cosm_new_file(struct sc_profile *profile, struct sc_card *card,
		unsigned int type, unsigned int num, struct sc_file **out)
{
	struct sc_file	*file;
	const char *_template = NULL, *desc = NULL;
	unsigned int structure = 0xFFFFFFFF;

	SC_FUNC_CALLED(card->ctx, 1);
	sc_debug(card->ctx, "cosm_new_file() type %X; num %i\n",type, num);
	while (1) {
		switch (type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
		case COSM_TYPE_PRKEY_RSA:
			desc = "RSA private key";
			_template = "template-private-key";
			structure = SC_CARDCTL_OBERTHUR_KEY_RSA_CRT;
			break;
		case SC_PKCS15_TYPE_PUBKEY_RSA:
		case COSM_TYPE_PUBKEY_RSA:
			desc = "RSA public key";
			_template = "template-public-key";
			structure = SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC;
			break;
		case SC_PKCS15_TYPE_PUBKEY_DSA:
			desc = "DSA public key";
			_template = "template-public-key";
			break;
		case SC_PKCS15_TYPE_CERT:
			desc = "certificate";
			_template = "template-certificate";
			break;
		case SC_PKCS15_TYPE_DATA_OBJECT:
			desc = "data object";
			_template = "template-public-data";
			break;
		}
		if (_template)
			break;
		/* If this is a specific type such as
		 * SC_PKCS15_TYPE_CERT_FOOBAR, fall back to
		 * the generic class (SC_PKCS15_TYPE_CERT)
		 */
		if (!(type & ~SC_PKCS15_TYPE_CLASS_MASK)) {
			sc_debug(card->ctx, "File type %X not supported by card driver", 
				type);
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		type &= SC_PKCS15_TYPE_CLASS_MASK;
	}

	sc_debug(card->ctx, "cosm_new_file() template %s; num %i\n",_template, num);
	if (sc_profile_get_file(profile, _template, &file) < 0) {
		sc_debug(card->ctx, "Profile doesn't define %s template '%s'\n",
				desc, _template);
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_NOT_SUPPORTED);
	}
 
	file->id |= (num & 0xFF);
	file->path.value[file->path.len-1] |= (num & 0xFF);
	if (file->type == SC_FILE_TYPE_INTERNAL_EF)   {
		file->ef_structure = structure;
	}

	sc_debug(card->ctx, "cosm_new_file() file size %i; ef type %i/%i; id %04X\n",file->size, 
			file->type, file->ef_structure, file->id);
	*out = file;

	SC_FUNC_RETURN(card->ctx, 1, SC_SUCCESS);
}


static int
cosm_get_temporary_public_key_file(struct sc_card *card,
		struct sc_file *prvkey_file, struct sc_file **pubkey_file)
{
	struct sc_context *ctx = card->ctx;
	const struct sc_acl_entry *entry = NULL;
	struct sc_file *file = NULL;
	int rv;

	SC_FUNC_CALLED(card->ctx, 1);
	if (!pubkey_file || !prvkey_file)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_INVALID_ARGUMENTS);

	file = sc_file_new();
	if (!file)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_OUT_OF_MEMORY);

	file->status = SC_FILE_STATUS_ACTIVATED;
        file->type = SC_FILE_TYPE_INTERNAL_EF;
	file->ef_structure = SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC;
	file->id = 0x1012;
	memcpy(&file->path, &prvkey_file->path, sizeof(file->path));
        file->path.value[file->path.len - 2] = 0x10;
	file->path.value[file->path.len - 1] = 0x12;
	file->size = prvkey_file->size;

	entry = sc_file_get_acl_entry(prvkey_file, SC_AC_OP_UPDATE);
	rv = sc_file_add_acl_entry(file, SC_AC_OP_UPDATE, entry->method, entry->key_ref);
	if (!rv)
		rv = sc_file_add_acl_entry(file, SC_AC_OP_PSO_ENCRYPT, SC_AC_NONE, 0);
	if (!rv)
		rv = sc_file_add_acl_entry(file, SC_AC_OP_PSO_VERIFY_SIGNATURE, SC_AC_NONE, 0);
	if (!rv)
		rv = sc_file_add_acl_entry(file, SC_AC_OP_EXTERNAL_AUTHENTICATE, SC_AC_NONE, 0);
	SC_TEST_RET(ctx, rv, "Failed to add ACL entry to the temporary public key file");

	*pubkey_file = file;

	SC_FUNC_RETURN(card->ctx, 1, rv);
}


static int
cosm_generate_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object, 
		struct sc_pkcs15_pubkey *pubkey)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_cardctl_oberthur_genkey_info args;
	struct sc_file *prkf = NULL, *tmpf = NULL;
	struct sc_path path;
	int rv = 0;

	SC_FUNC_CALLED(ctx, 1);

	if (object->type != SC_PKCS15_TYPE_PRKEY_RSA)
		SC_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Generate key failed: RSA only supported");

	path = key_info->path;
	path.len -= 2;

	rv = sc_select_file(p15card->card, &path, &tmpf);
	SC_TEST_RET(ctx, rv, "Cannot generate key: failed to select private object DF");
	
	rv = sc_pkcs15init_authenticate(profile, p15card, tmpf, SC_AC_OP_CRYPTO); 
	SC_TEST_RET(ctx, rv, "Cannot generate key: 'CRYPTO' authentication failed");
	
	rv = sc_pkcs15init_authenticate(profile, p15card, tmpf, SC_AC_OP_CREATE);
	SC_TEST_RET(ctx, rv, "Cannot generate key: 'CREATE' authentication failed");
	
	sc_file_free(tmpf);

	rv = sc_select_file(p15card->card, &key_info->path, &prkf);
	SC_TEST_RET(ctx, rv, "Failed to generate key: cannot select private key file");
	
	/* In the private key DF create the temporary public RSA file. */
	rv = cosm_get_temporary_public_key_file(p15card->card, prkf, &tmpf);
	SC_TEST_RET(ctx, rv, "Error while getting temporary public key file");

	rv = sc_pkcs15init_create_file(profile, p15card, tmpf);
	SC_TEST_RET(ctx, rv, "cosm_generate_key() failed to create temporary public key EF");
	
	memset(&args, 0, sizeof(args));
	args.id_prv = prkf->id;
	args.id_pub = tmpf->id;
	args.exponent = 0x10001;
	args.key_bits = key_info->modulus_length;
	args.pubkey_len = key_info->modulus_length / 8;
	args.pubkey = (unsigned char *) malloc(key_info->modulus_length / 8);
	if (!args.pubkey)
		SC_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "cosm_generate_key() cannot allocate pubkey");
	
	rv = sc_card_ctl(p15card->card, SC_CARDCTL_OBERTHUR_GENERATE_KEY, &args);
	SC_TEST_RET(ctx, rv, "cosm_generate_key() CARDCTL_OBERTHUR_GENERATE_KEY failed");
	
	/* extract public key */
	pubkey->algorithm = SC_ALGORITHM_RSA;
	pubkey->u.rsa.modulus.len   = key_info->modulus_length / 8;
	pubkey->u.rsa.modulus.data  = (unsigned char *) malloc(key_info->modulus_length / 8);
	if (!pubkey->u.rsa.modulus.data)
		SC_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "cosm_generate_key() cannot allocate modulus buf");
	
	/* FIXME and if the exponent length is not 3? */
	pubkey->u.rsa.exponent.len  = 3;
	pubkey->u.rsa.exponent.data = (unsigned char *) malloc(3);
	if (!pubkey->u.rsa.exponent.data) 
		SC_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "cosm_generate_key() cannot allocate exponent buf");
	memcpy(pubkey->u.rsa.exponent.data, "\x01\x00\x01", 3);
	memcpy(pubkey->u.rsa.modulus.data, args.pubkey, args.pubkey_len);

	key_info->key_reference = prkf->path.value[prkf->path.len - 1] & 0xFF;
	key_info->path = prkf->path;
	
	sc_debug(ctx, "cosm_generate_key() now delete temporary public key\n");
	rv =  cosm_delete_file(p15card, profile, tmpf);
	
	sc_file_free(tmpf);
	sc_file_free(prkf);

	SC_FUNC_RETURN(ctx, 1, rv);
}


/*
 * Create private key file
 */
static int
cosm_create_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	int rv = 0;

	SC_FUNC_CALLED(ctx, 1);
	if (object->type != SC_PKCS15_TYPE_PRKEY_RSA)
		SC_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Create key failed: RSA only supported");

	sc_debug(ctx, "create private key ID:%s\n",  sc_pkcs15_print_id(&key_info->id));
	/* Here, the path of private key file should be defined.
	 * Neverthelles, we need to instanciate private key to get the ACLs. */
	rv = cosm_new_file(profile, p15card->card, SC_PKCS15_TYPE_PRKEY_RSA, key_info->key_reference, &file);
	SC_TEST_RET(ctx, rv, "Cannot create key: failed to allocate new key object");

	file->size = key_info->modulus_length;
	memcpy(&file->path, &key_info->path, sizeof(file->path));
	file->id = file->path.value[file->path.len - 2] * 0x100 
				+ file->path.value[file->path.len - 1];

	sc_debug(ctx, "Path of private key file to create %s\n", sc_print_path(&file->path));

	rv = sc_select_file(p15card->card, &file->path, NULL);
	if (rv == 0)   {
		rv = cosm_delete_file(p15card, profile, file);
		SC_TEST_RET(ctx, rv, "Failed to delete private key file");
	}
	else if (rv != SC_ERROR_FILE_NOT_FOUND)    {
		SC_TEST_RET(ctx, rv, "Select private key file error");
	}
		
	rv = sc_pkcs15init_create_file(profile, p15card, file);
	SC_TEST_RET(ctx, rv, "Failed to create private key file");

	key_info->key_reference = file->path.value[file->path.len - 1];

	sc_file_free(file);

	SC_FUNC_RETURN(ctx, 1, rv);
}


/*
 * Store a private key
 */
static int
cosm_store_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object, 
		struct sc_pkcs15_prkey *prkey)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	struct sc_cardctl_oberthur_updatekey_info update_info;
	int rv = 0;

	SC_FUNC_CALLED(ctx, 1);
	if (object->type != SC_PKCS15_TYPE_PRKEY_RSA || prkey->algorithm != SC_ALGORITHM_RSA)
		SC_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Store key failed: RSA only supported");

	sc_debug(ctx, "store key with ID:%s and path:%s\n", sc_pkcs15_print_id(&key_info->id),
		       	sc_print_path(&key_info->path));

	rv = sc_select_file(p15card->card, &key_info->path, &file);
	SC_TEST_RET(ctx, rv, "Cannot store key: select key file failed");
	
	rv = sc_pkcs15init_authenticate(profile, p15card, file, SC_AC_OP_UPDATE);
	SC_TEST_RET(ctx, rv, "No authorisation to store private key");

	if (key_info->id.len > sizeof(update_info.id))
		 SC_FUNC_RETURN(ctx, 1, SC_ERROR_INVALID_ARGUMENTS);
	
	memset(&update_info, 0, sizeof(update_info));
	update_info.type = SC_CARDCTL_OBERTHUR_KEY_RSA_CRT;
	update_info.data = (void *)&prkey->u.rsa;
	update_info.data_len = sizeof(void *);
	update_info.id_len = key_info->id.len;
	memcpy(update_info.id, key_info->id.value, update_info.id_len);
		
	rv = sc_card_ctl(p15card->card, SC_CARDCTL_OBERTHUR_UPDATE_KEY, &update_info);
	SC_TEST_RET(ctx, rv, "Cannot update private key");
	
	if (file) 
		sc_file_free(file);

	SC_FUNC_RETURN(ctx, 1, rv);
}


static struct sc_pkcs15init_operations 
sc_pkcs15init_oberthur_operations = {
	cosm_erase_card,
	NULL,				/* init_card  */
	cosm_create_dir,		/* create_dir */
	NULL,				/* create_domain */
	cosm_select_pin_reference,
	cosm_create_pin,
	NULL,				/* select_key_reference */
	cosm_create_key,		/* create_key */
	cosm_store_key,			/* store_key */
	cosm_generate_key,		/* generate_key */
	NULL, 
	NULL,				/* encode private/public key */
	NULL,				/* finalize_card */
	NULL				/* delete_object */	
};

struct sc_pkcs15init_operations *
sc_pkcs15init_get_oberthur_ops(void)
{   
	return &sc_pkcs15init_oberthur_operations;
}
