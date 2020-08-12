/*
 * Copyright (C) 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-plugin-vfuncs.h"
#include "fu-efivar.h"
#include "fu-hash.h"
#include "fu-uefi-dbx-common.h"
#include "fu-efi-signature-parser.h"

struct FuPluginData {
	gchar			*fn;
};

void
fu_plugin_init (FuPlugin *plugin)
{
	fu_plugin_alloc_data (plugin, sizeof (FuPluginData));
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_free (data->fn);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	data->fn = fu_uefi_dbx_get_dbxupdate (error);
	if (data->fn == NULL)
		return FALSE;
	g_debug ("using %s", data->fn);
	return TRUE;
}

void
fu_plugin_add_security_attrs (FuPlugin *plugin, FuSecurityAttrs *attrs)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	GPtrArray *items;
	gsize bufsz = 0;
	guint missing_cnt = 0;
	g_autofree guint8 *buf_system = NULL;
	g_autofree guint8 *buf_update = NULL;
	g_autoptr(FuEfiSignatureList) dbx_system = NULL;
	g_autoptr(FuEfiSignatureList) dbx_update = NULL;
	g_autoptr(FwupdSecurityAttr) attr = NULL;
	g_autoptr(GError) error_local = NULL;

	/* create attr */
	attr = fwupd_security_attr_new (FWUPD_SECURITY_ATTR_ID_UEFI_DBX);
	fwupd_security_attr_set_plugin (attr, fu_plugin_get_name (plugin));
	fwupd_security_attr_set_level (attr, FWUPD_SECURITY_ATTR_LEVEL_CRITICAL);
	fu_security_attrs_append (attrs, attr);

	/* no binary blob */
	if (!fu_plugin_get_enabled (plugin)) {
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_FOUND);
		fwupd_security_attr_set_url (attr, "https://github.com/fwupd/fwupd/wiki/Missingdbx");
		return;
	}

	/* get update dbx */
	if (!g_file_get_contents (data->fn, (gchar **) &buf_update, &bufsz, &error_local)) {
		g_warning ("failed to load %s: %s", data->fn, error_local->message);
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_VALID);
		return;
	}
	dbx_update = fu_efi_signature_parser_one (buf_update, bufsz,
						  FU_EFI_SIGNATURE_PARSER_FLAGS_IGNORE_HEADER,
						  &error_local);
	if (dbx_update == NULL) {
		g_warning ("failed to parse %s: %s", data->fn, error_local->message);
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_VALID);
		return;
	}

	/* get system dbx */
	if (!fu_efivar_get_data (FU_EFIVAR_GUID_SECURITY_DATABASE, "dbx",
				 &buf_system, &bufsz, NULL, &error_local)) {
		g_warning ("failed to load EFI dbx: %s", error_local->message);
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_VALID);
		return;
	}
	dbx_system = fu_efi_signature_parser_one (buf_system, bufsz,
						  FU_EFI_SIGNATURE_PARSER_FLAGS_NONE,
						  &error_local);
	if (dbx_system == NULL) {
		g_warning ("failed to parse EFI dbx: %s", error_local->message);
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_VALID);
		return;
	}

	/* look for each checksum in the update in the system version */
	items = fu_efi_signature_list_get_all (dbx_update);
	for (guint i = 0; i < items->len; i++) {
		FuEfiSignature *item = g_ptr_array_index (items, i);
		if (!fu_efi_signature_list_has_checksum (dbx_system, fu_efi_signature_get_checksum (item))) {
			g_debug ("%s missing from the system DBX",
				 fu_efi_signature_get_checksum (item));
			missing_cnt += 1;
		}
	}

	/* add security attribute */
	if (missing_cnt > 0) {
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_FOUND);
		return;
	}

	/* success */
	fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_SUCCESS);
	fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_FOUND);
}
