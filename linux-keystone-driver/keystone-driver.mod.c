#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x607ccb0a, "param_ops_ullong" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x426d4786, "misc_deregister" },
	{ 0x7bbb0e32, "idr_find" },
	{ 0xe922053a, "param_ops_ulong" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x9bd5dbb, "finish_wait" },
	{ 0xe422adc0, "pgtable_l5_enabled" },
	{ 0x9372050, "param_ops_long" },
	{ 0x984d47c1, "remap_pfn_range" },
	{ 0x37a0cba, "kfree" },
	{ 0x9d307a20, "prepare_to_wait_event" },
	{ 0x5c972bf6, "__wake_up" },
	{ 0x4302d0eb, "free_pages" },
	{ 0x92997ed8, "_printk" },
	{ 0xbb21527, "kernel_map" },
	{ 0x1000e51, "schedule" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x6a5cb5ee, "__get_free_pages" },
	{ 0x800473f, "__cond_resched" },
	{ 0xe6deef9e, "idr_remove" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0x1d7de340, "idr_alloc" },
	{ 0x4401802d, "mutex_lock" },
	{ 0x888f932c, "dma_alloc_attrs" },
	{ 0xbe0c6fe, "pgtable_l4_enabled" },
	{ 0xd8925a5d, "sbi_ecall" },
	{ 0xfb578fc5, "memset" },
	{ 0x48e39d97, "misc_register" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0x3c3ff9fd, "sprintf" },
	{ 0x6c5be10d, "dma_free_attrs" },
	{ 0xd48458d4, "mutex_unlock" },
	{ 0x4c21c277, "kmalloc_trace" },
	{ 0xe08fbfe7, "param_ops_int" },
	{ 0xf9a482f9, "msleep" },
	{ 0x9177c4aa, "kmalloc_caches" },
	{ 0x27cc7e3, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "EEA8FD87396A204C7199ACB");
