/*
 * Intel ACPI functions
 *
 * _DSM related code stolen from nouveau_acpi.c.
 */
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/hardirq.h>

#include <linux/vga_switcheroo.h>
#include <drm/drmP.h>
#include "i915_drv.h"

#define INTEL_DSM_REVISION_ID 1 /* For Calpella anyway... */
#define INTEL_DSM_FN_PLATFORM_MUX_INFO 1 /* No args */

static struct intel_dsm_priv {
	acpi_handle dhandle;
} intel_dsm_priv;

static const u8 intel_dsm_guid[] = {
	0xd3, 0x73, 0xd8, 0x7e,
	0xd0, 0xc2,
	0x4f, 0x4e,
	0xa8, 0x54,
	0x0f, 0x13, 0x17, 0xb0, 0x1c, 0x2c
};

static char *intel_dsm_port_name(u8 id)
{
	switch (id) {
	case 0:
		return "Reserved";
	case 1:
		return "Analog VGA";
	case 2:
		return "LVDS";
	case 3:
		return "Reserved";
	case 4:
		return "HDMI/DVI_B";
	case 5:
		return "HDMI/DVI_C";
	case 6:
		return "HDMI/DVI_D";
	case 7:
		return "DisplayPort_A";
	case 8:
		return "DisplayPort_B";
	case 9:
		return "DisplayPort_C";
	case 0xa:
		return "DisplayPort_D";
	case 0xb:
	case 0xc:
	case 0xd:
		return "Reserved";
	case 0xe:
		return "WiDi";
	default:
		return "bad type";
	}
}

static char *intel_dsm_mux_type(u8 type)
{
	switch (type) {
	case 0:
		return "unknown";
	case 1:
		return "No MUX, iGPU only";
	case 2:
		return "No MUX, dGPU only";
	case 3:
		return "MUXed between iGPU and dGPU";
	default:
		return "bad type";
	}
}



/**
 * acpi_evaluate_dsm - evaluate device's _DSM method
 * @handle: ACPI device handle
 * @uuid: UUID of requested functions, should be 16 bytes
 * @rev: revision number of requested function
 * @func: requested function number
 * @argv4: the function specific parameter
 *
 * Evaluate device's _DSM method with specified UUID, revision id and
 * function number. Caller needs to free the returned object.
 *
 * Though ACPI defines the fourth parameter for _DSM should be a package,
 * some old BIOSes do expect a buffer or an integer etc.
 */
union acpi_object *
acpi_evaluate_dsm(acpi_handle handle, const u8 *uuid, int rev, int func,
                  union acpi_object *argv4)
{
        acpi_status ret;
        struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
        union acpi_object params[4];
        struct acpi_object_list input = {
                .count = 4,
                .pointer = params,
        };

        params[0].type = ACPI_TYPE_BUFFER;
        params[0].buffer.length = 16;
        params[0].buffer.pointer = (char *)uuid;
        params[1].type = ACPI_TYPE_INTEGER;
        params[1].integer.value = rev;
        params[2].type = ACPI_TYPE_INTEGER;
        params[2].integer.value = func;
        if (argv4) {
                params[3] = *argv4;
        } else {
                params[3].type = ACPI_TYPE_PACKAGE;
                params[3].package.count = 0;
                params[3].package.elements = NULL;
        }

        ret = acpi_evaluate_object(handle, "_DSM", &input, &buf);
        if (ACPI_SUCCESS(ret))
                return (union acpi_object *)buf.pointer;

        if (ret != AE_NOT_FOUND)
                acpi_handle_warn(handle,
                                "failed to evaluate _DSM (0x%x)\n", ret);

        return NULL;
}
EXPORT_SYMBOL(acpi_evaluate_dsm);




static inline union acpi_object *
acpi_evaluate_dsm_typed(acpi_handle handle, const u8 *uuid, int rev, int func,
                        union acpi_object *argv4, acpi_object_type type)
{
        union acpi_object *obj;

        obj = acpi_evaluate_dsm(handle, uuid, rev, func, argv4);
        if (obj && obj->type != type) {
                ACPI_FREE(obj);
                obj = NULL;
        }

        return obj;
}


static void intel_dsm_platform_mux_info(void)
{
	int i;
	union acpi_object *pkg, *connector_count;

	pkg = acpi_evaluate_dsm_typed(intel_dsm_priv.dhandle, intel_dsm_guid,
			INTEL_DSM_REVISION_ID, INTEL_DSM_FN_PLATFORM_MUX_INFO,
			NULL, ACPI_TYPE_PACKAGE);
	if (!pkg) {
		DRM_DEBUG_DRIVER("failed to evaluate _DSM\n");
		return;
	}

	connector_count = &pkg->package.elements[0];
	DRM_DEBUG_DRIVER("MUX info connectors: %lld\n",
		  (unsigned long long)connector_count->integer.value);
	for (i = 1; i < pkg->package.count; i++) {
		union acpi_object *obj = &pkg->package.elements[i];
		union acpi_object *connector_id = &obj->package.elements[0];
		union acpi_object *info = &obj->package.elements[1];
		DRM_DEBUG_DRIVER("Connector id: 0x%016llx\n",
			  (unsigned long long)connector_id->integer.value);
		DRM_DEBUG_DRIVER("  port id: %s\n",
		       intel_dsm_port_name(info->buffer.pointer[0]));
		DRM_DEBUG_DRIVER("  display mux info: %s\n",
		       intel_dsm_mux_type(info->buffer.pointer[1]));
		DRM_DEBUG_DRIVER("  aux/dc mux info: %s\n",
		       intel_dsm_mux_type(info->buffer.pointer[2]));
		DRM_DEBUG_DRIVER("  hpd mux info: %s\n",
		       intel_dsm_mux_type(info->buffer.pointer[3]));
	}

	ACPI_FREE(pkg);
}

/**
 * acpi_check_dsm - check if _DSM method supports requested functions.
 * @handle: ACPI device handle
 * @uuid: UUID of requested functions, should be 16 bytes at least
 * @rev: revision number of requested functions
 * @funcs: bitmap of requested functions
 * @exclude: excluding special value, used to support i915 and nouveau
 *
 * Evaluate device's _DSM method to check whether it supports requested
 * functions. Currently only support 64 functions at maximum, should be
 * enough for now.
 */
bool acpi_check_dsm(acpi_handle handle, const u8 *uuid, int rev, u64 funcs)
{
        int i;
        u64 mask = 0;
        union acpi_object *obj;

        if (funcs == 0)
                return false;

        obj = acpi_evaluate_dsm(handle, uuid, rev, 0, NULL);
        if (!obj)
                return false;

        /* For compatibility, old BIOSes may return an integer */
        if (obj->type == ACPI_TYPE_INTEGER)
                mask = obj->integer.value;
        else if (obj->type == ACPI_TYPE_BUFFER)
                for (i = 0; i < obj->buffer.length && i < 8; i++)
                        mask |= (((u8)obj->buffer.pointer[i]) << (i * 8));
        ACPI_FREE(obj);

        /*
         * Bit 0 indicates whether there's support for any functions other than
         * function 0 for the specified UUID and revision.
         */
        if ((mask & 0x1) && (mask & funcs) == funcs)
                return true;

        return false;
}
EXPORT_SYMBOL(acpi_check_dsm);



static bool intel_dsm_pci_probe(struct pci_dev *pdev)
{
	acpi_handle dhandle;

	dhandle = ACPI_HANDLE(&pdev->dev);
	if (!dhandle)
		return false;

	if (!acpi_check_dsm(dhandle, intel_dsm_guid, INTEL_DSM_REVISION_ID,
			    1 << INTEL_DSM_FN_PLATFORM_MUX_INFO)) {
		DRM_DEBUG_KMS("no _DSM method for intel device\n");
		return false;
	}

	intel_dsm_priv.dhandle = dhandle;
	intel_dsm_platform_mux_info();

	return true;
}

static bool intel_dsm_detect(void)
{
	char acpi_method_name[255] = { 0 };
	struct acpi_buffer buffer = {sizeof(acpi_method_name), acpi_method_name};
	struct pci_dev *pdev = NULL;
	bool has_dsm = false;
	int vga_count = 0;

	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, pdev)) != NULL) {
		vga_count++;
		has_dsm |= intel_dsm_pci_probe(pdev);
	}

	if (vga_count == 2 && has_dsm) {
		acpi_get_name(intel_dsm_priv.dhandle, ACPI_FULL_PATHNAME, &buffer);
		DRM_DEBUG_DRIVER("VGA switcheroo: detected DSM switching method %s handle\n",
				 acpi_method_name);
		return true;
	}

	return false;
}

void intel_register_dsm_handler(void)
{
	if (!intel_dsm_detect())
		return;
}

void intel_unregister_dsm_handler(void)
{
}
