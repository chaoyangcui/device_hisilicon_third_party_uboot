/*
 * hi3516dv300.c
 *
 * The board init for hisilicon
 *
 * Copyright (c) 2020 HiSilicon (Shanghai) Technologies CO., LIMITED.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <config.h>
#include <common.h>
#include <asm/io.h>
#include <asm/arch/platform.h>
#include <spi_flash.h>
#include <linux/mtd/mtd.h>
#include <nand.h>
#include <netdev.h>
#include <mmc.h>
#include <asm/sections.h>
#include <hicpu_common.h>
#include <asm/mach-types.h>
#include <cpu_func.h>
#ifdef CONFIG_AUTO_OTA_UPDATE
#include <fat.h>
#endif /* CONFIG_AUTO_OTA_UPDATE */
#include <env.h>
#include <string.h>

#ifndef CONFIG_SYS_DCACHE_OFF
void enable_caches(void)
{
	/* Enable D-cache. I-cache is already enabled in start.S */
	dcache_enable();
}
#endif
static int boot_media = BOOT_MEDIA_UNKNOWN;

int get_boot_media(void)
{
	unsigned int reg_val, boot_mode, spi_device_mode;
	int boot_media = BOOT_MEDIA_UNKNOWN;

	reg_val = readl(SYS_CTRL_REG_BASE + REG_SYSSTAT);
	boot_mode = get_sys_boot_mode(reg_val);

	switch (boot_mode) {
	case BOOT_FROM_SPI:
		spi_device_mode = get_spi_device_type(reg_val);
		if (spi_device_mode)
			boot_media = BOOT_MEDIA_NAND;
		else
			boot_media = BOOT_MEDIA_SPIFLASH;
		break;
	case BOOT_FROM_EMMC:
		boot_media = BOOT_MEDIA_EMMC;
		break;
	default:
		boot_media = BOOT_MEDIA_UNKNOWN;
		break;
	}
	return boot_media;
}

#if defined(CONFIG_SHOW_BOOT_PROGRESS)
void show_boot_progress(int progress)
{
	printf("Boot reached stage %d\n", progress);
}
#endif

#define COMP_MODE_ENABLE ((unsigned int)0x0000EAEF)

static inline void delay(unsigned long loops)
{
	__asm__ volatile("1:\n"
			"subs %0, %1, #1\n"
			"bne 1b" : "=r"(loops) : "0"(loops));
}


int get_text_base(void)
{
	return CONFIG_SYS_TEXT_BASE;
}

static void boot_flag_init(void)
{
	unsigned int reg, boot_mode, spi_device_mode;

	/* get boot mode */
	reg = __raw_readl(SYS_CTRL_REG_BASE + REG_SYSSTAT);
	boot_mode = get_sys_boot_mode(reg);

	switch (boot_mode) {
	case BOOT_FROM_SPI:
		spi_device_mode = get_spi_device_type(reg);
		if (spi_device_mode)
			boot_media = BOOT_MEDIA_NAND;
		else
			boot_media = BOOT_MEDIA_SPIFLASH;
		break;
	case BOOT_FROM_EMMC:    /* emmc mode */
		boot_media = BOOT_MEDIA_EMMC;
		break;
	default:
		boot_media = BOOT_MEDIA_UNKNOWN;
		break;
	}
}

int board_early_init_f(void)
{
	return 0;
}

#define UBOOT_DATA_ADDR     0x81000000UL
#define UBOOT_DATA_SIZE     0x80000UL
int data_to_spiflash(void)
{
	static struct spi_flash *flash = NULL;
	void *buf = NULL;

	unsigned int val;

	/* 0:bus  0:cs  1000000:max_hz  0x3:spi_mode */
	flash = spi_flash_probe(0, 0, 1000000, 0x3);
	if (!flash) {
		printf("Failed to initialize SPI flash\n");
		return -1;  /* -1:failed */
	}

	/* erase the address range. */
	printf("Spi flash erase...\n");
	val = spi_flash_erase(flash, NUM_0, UBOOT_DATA_SIZE);
	if (val) {
		printf("SPI flash sector erase failed\n");
		return 1; /* 1:failed */
	}

	buf = map_physmem((unsigned long)UBOOT_DATA_ADDR,
			UBOOT_DATA_SIZE, MAP_WRBACK);
	if (!buf) {
		puts("Failed to map physical memory\n");
		return 1; /* 1:failed */
	}

	/* copy the data from RAM to FLASH */
	printf("Spi flash write...\n");
	val = flash->write(flash, NUM_0, UBOOT_DATA_SIZE, buf);
	if (val) {
		printf("SPI flash write failed, return %u\n",
				val);
		unmap_physmem(buf, UBOOT_DATA_SIZE);
		return 1; /* 1:failed */
	}

	unmap_physmem(buf, UBOOT_DATA_SIZE);
	return 0; /* 0:success */
}

int data_to_nandflash(void)
{
	struct mtd_info *nand_flash = NULL;
	void *buf = NULL;
	size_t length = UBOOT_DATA_SIZE;
	unsigned int val;

	nand_flash = nand_info[0];

	printf("Nand flash erase...\n");
	val = nand_erase(nand_flash, 0, UBOOT_DATA_SIZE);
	if (val) {
		printf("Nand flash erase failed\n");
		return 1;
	}

	buf = map_physmem((unsigned long)UBOOT_DATA_ADDR,
			UBOOT_DATA_SIZE, MAP_WRBACK);
	if (!buf) {
		puts("Failed to map physical memory\n");
		return 1;
	}

	printf("Nand flash write...\n");
	val = nand_write(nand_flash, 0, &length, buf);
	if (val) {
		printf("Nand flash write failed, return %u\n",
				val);
		unmap_physmem(buf, UBOOT_DATA_SIZE);
		return 1;
	}

	unmap_physmem(buf, UBOOT_DATA_SIZE);
	return 0;
}

int data_to_emmc(void)
{
	struct mmc *mmc = find_mmc_device(0);
	void *buf = NULL;

	if (!mmc)
		return 1;

	(void)mmc_init(mmc);

	buf = map_physmem((unsigned long)UBOOT_DATA_ADDR,
			UBOOT_DATA_SIZE, MAP_WRBACK);
	if (!buf) {
		puts("Failed to map physical memory\n");
		return 1;
	}

	printf("MMC write...\n");
	blk_dwrite(mmc_get_blk_desc(mmc), 0, (UBOOT_DATA_SIZE >> NUM_9), buf);
	unmap_physmem(buf, UBOOT_DATA_SIZE);
	return 0;
}
int save_bootdata_to_flash(void)
{
	unsigned int sd_update_flag = 0;
	int ret = 0;
	sd_update_flag = readl(REG_BASE_SCTL + REG_SC_GEN4);
	if (sd_update_flag == START_MAGIC) {
#if defined(CONFIG_HIFMC)
		if (boot_media == BOOT_MEDIA_SPIFLASH) {
			ret = data_to_spiflash();
			if (ret != 0)
				return ret;
		}
		if (boot_media == BOOT_MEDIA_NAND) {
			ret = data_to_nandflash();
			if (ret != 0)
				return ret;
		}
#endif
#if defined(CONFIG_SUPPORT_EMMC_BOOT)
		if (boot_media == BOOT_MEDIA_EMMC) {
			ret = data_to_emmc();
			if (ret != 0)
				return ret;
		}
#endif

		printf("update success!\n");
	}

	return 0;
}

int auto_update_flag = 0;
int bare_chip_program = 0;

#define REG_BASE_GPIO0          0x120d0000
#define GPIO0_0_DATA_OFST       0x4
#define GPIO_DIR_OFST           0x400

int is_bare_program(void)
{
	return 0;
}

#ifdef CONFIG_AUTO_OTA_UPDATE
static int g_dev_num;
static bool is_mmc_valid(void)
{
	struct mmc *mmc = find_mmc_device(g_dev_num);
	if (!mmc) {
		printf("No mmc %d driver found!\n", g_dev_num);
		return false;
	}
	if (((unsigned long)mmc->block_dev.vendor[0] == 0) ||
		((unsigned long)mmc->block_dev.product[0] == 0)) {
		printf("No SD card found!\n");
		return false;
	}
	return true;
}

static bool is_ota_tag_valid(const char *path)
{
	char buf[64] = {0}; /* 32bytes for max in OTA_TAG_FILE */
	const char *info = "package_type:ota";
	const int len = strlen(info);
	long sz = file_fat_read(path, (void *)buf, sizeof(buf));

	if (sz < len) {
		printf("%s: not exist, or len %ld invalid\n", path, sz);
		return false;
	}

	if (strncmp(info, buf, len) != 0) {
		printf("%s info invalid\n", path);
		return false;
	}
	return true;
}

static bool is_ota(void)
{
	char name[] = "mmc";
	struct blk_desc *stor_dev = NULL;
	bool valid = false;

	if (!is_mmc_valid()) {
		printf("MMC not valid\n");
		return false;
	}

	stor_dev = blk_get_dev(name, g_dev_num);
	if (stor_dev == NULL) {
		printf("Unknow device type!\n");
		return false;
	}

	if (fat_register_device(stor_dev, 1) != 0) {
		printf("Unable to use %s for fat\n", name);
		return false;
	}

	if (file_fat_detectfs() != 0) {
		printf("Fat-detectfs failed\n");
		return false;
	}

	valid = is_ota_tag_valid("/update/OTA.tag");
	printf("OTA.tag valid %d\n", valid);
	return valid;
}
#endif /* CONFIG_AUTO_OTA_UPDATE */

static int is_auto_ota_update(void)
{
#ifdef CONFIG_AUTO_OTA_UPDATE
	if (is_ota())
		return 1;
#endif
	return 0;
}

int is_auto_update(void)
{
#if (defined CONFIG_AUTO_SD_UPDATE) || (defined CONFIG_AUTO_USB_UPDATE)
	/* to add some judgement if neccessary */
	unsigned int  val[NUM_3];

	writel(0, REG_BASE_GPIO0 + GPIO_DIR_OFST);

	val[NUM_0] = readl(REG_BASE_GPIO0 + GPIO0_0_DATA_OFST);
	if (val[NUM_0])
		return is_auto_ota_update();

	udelay(10000); /* delay 10000 us */
	val[NUM_1] = readl(REG_BASE_GPIO0 + GPIO0_0_DATA_OFST);
	udelay(10000); /* delay 10000 us */
	val[NUM_2] = readl(REG_BASE_GPIO0 + GPIO0_0_DATA_OFST);
	udelay(10000); /* delay 10000 us */

	if (val[NUM_0] == val[NUM_1] && val[NUM_1] == val[NUM_2] && val[NUM_0] == NUM_0)
		return 1;    /* update enable */
	else
		return is_auto_ota_update();

#else
	return is_auto_ota_update();
#endif
}

#define EMMC_SECTOR_SIZE (1<<9)
#define M_1 (1024*1024)
#define MISC_LOCATION 36
#define MAX_COMMAND_SIZE 20
#define MAX_UPDATE_SIZE 100
struct UpdateMessage {
    char command[MAX_COMMAND_SIZE];
    char update[MAX_UPDATE_SIZE];
};

static struct mmc *mmc;
struct mmc *MmcBlkDevInit(int dev)
{
    struct mmc *mmcDev;

    mmcDev = find_mmc_device(dev);
    if (!mmcDev) {
        printf("no mmc device at slot %x\n", dev);
        return NULL;
    }

    if (mmc_init(mmcDev)) {
        return NULL;
    }
    return mmcDev;
}

static int MmcBlkRead(const struct mmc *mmcDev, char *buffer, u32 blk, u32 cnt)
{
    ulong start = (ulong)buffer;

    debug("\nMMC read: block # 0x%x, count 0x%x  to %p... ", blk, cnt, buffer);

    u32 n = blk_dread(mmc_get_blk_desc(mmcDev), blk, cnt, buffer);
    /* invalidate cache after read via dma */
    invalidate_dcache_range(start, start + cnt * EMMC_SECTOR_SIZE);
    debug("%d blocks read: %s\n", n, (n == cnt) ? "OK" : "ERROR");
    printf("@@@ %d blocks read: %s\n", n, (n == cnt) ? "OK" : "ERROR");

    return (n == cnt) ? 0 : -EIO;
}

#define NUM_BASE 10
int BlkDevRead(char *buffer, u32 blk, u32 cnt)
{
    if (!mmc) {
        int devNo = env_get_ulong("mmcdev", NUM_BASE, 0);
        mmc = MmcBlkDevInit(devNo);
        if (!mmc) {
            return -ENODEV;
        }
    }
    return MmcBlkRead(mmc, buffer, blk, cnt);
}

static int g_isRecovery = 0;
#define EMMC_SECTOR_CNT 5
#define ARG_SZ 1000
#define MMC_LENGTH 7
#define UPDATE_BOOT_LENGTH 12
#define MIN_BOOTARGS_LENGTH 10
#define PARTITION_INFO_POS 1144
#define PARTITION_INFO_MAX_LENGTH 256

char g_bootArgsStr[ARG_SZ];

static void ChangeBootArgs()            // get bootargs from emmc
{
    char *emmcBootArgs = env_get("bootargs");
    if (!emmcBootArgs) {
        printf("@@@ bootArgs from emmc is bad = NULL\n");
        return;
    }
    int emmcBootArgsLen = strlen(emmcBootArgs);
    if (emmcBootArgsLen < MIN_BOOTARGS_LENGTH) {
        printf("@@@ bootArgs from emmc is bad = %s, len=%d\n", emmcBootArgs, emmcBootArgsLen);
        return;
    }
    if (!g_isRecovery) {                 // hos
        memset(g_bootArgsStr, 0, ARG_SZ);
        memcpy(g_bootArgsStr, emmcBootArgs, emmcBootArgsLen);
        printf("@@@ bootArgs final from emmc = %s\n", g_bootArgsStr);
    } else {
        printf("@@@ bootArgs final from misc = %s\n", g_bootArgsStr);
    }
}

static int EmmcInitParam()              // get "boot_updater" string in misc,then set env
{
    const char rebootHead[] = "mem=640M console=ttyAMA0,115200 mmz=anonymous,0,0xA8000000,384M "
        "clk_ignore_unused androidboot.selinux=permissive skip_initramfs rootdelay=10 init=/init "
        "root=/dev/mmcblk0p5 rootfstype=ext4 ro blkdevparts=";
    const char defaultRebootStr[] = "mem=640M console=ttyAMA0,115200 mmz=anonymous,0,0xA8000000,384M "
        "clk_ignore_unused androidboot.selinux=permissive skip_initramfs rootdelay=10 init=/init root=/dev/mmcblk0p5 "
        "rootfstype=ext4 ro blkdevparts=mmcblk0:1M(boot),15M(kernel),20M(updater),"
        "1M(misc),3307M(system),256M(vendor),-(userdata)";
    const char updaterHead[] = "mem=640M console=ttyAMA0,115200 mmz=anonymous,0,0xA8000000,384M clk_ignore_unused "
        "androidboot.selinux=permissive skip_initramfs "
        "rootdelay=10 init=/init root=/dev/mmcblk0p3 rootfstype=ext4 rw blkdevparts=";
    const char defaultUpdaterStr[] = "mem=640M console=ttyAMA0,115200 mmz=anonymous,0,0xA8000000,384M "
        "clk_ignore_unused androidboot.selinux=permissive skip_initramfs "
        "rootdelay=10 init=/init root=/dev/mmcblk0p3 rootfstype=ext4 rw blkdevparts=mmcblk0:1M(boot),"
        "15M(kernel),20M(updater),1M(misc),3307M(system),256M(vendor),-(userdata)";
    char block2[EMMC_SECTOR_SIZE*EMMC_SECTOR_CNT];
    if (BlkDevRead(block2, MISC_LOCATION*(M_1/EMMC_SECTOR_SIZE), EMMC_SECTOR_CNT) < 0) {
        return -1;
    }

    struct UpdateMessage *p = (struct UpdateMessage *)block2;
    block2[MAX_COMMAND_SIZE - 1] = block2[MAX_COMMAND_SIZE + MAX_UPDATE_SIZE - 1] =
        block2[EMMC_SECTOR_SIZE * EMMC_SECTOR_CNT - 1] = 0;
    p->command[0] = p->command[0] == ((char)-1) ? 0 : p->command[0];
    p->update[0] = p->update[0] == ((char)-1) ? 0 : p->update[0];
    block2[PARTITION_INFO_POS] = block2[PARTITION_INFO_POS] == (char)-1 ? 0 : block2[PARTITION_INFO_POS];
    block2[PARTITION_INFO_POS + PARTITION_INFO_MAX_LENGTH] = 0;

    g_isRecovery = memcmp(p->command, "boot_updater", UPDATE_BOOT_LENGTH) ? 0 : 1;
    unsigned int partitionStrLen = strlen(&block2[PARTITION_INFO_POS]);

    if (memcmp(&block2[PARTITION_INFO_POS], "mmcblk0", MMC_LENGTH)) {
        if (g_isRecovery) {
            memcpy(g_bootArgsStr, defaultUpdaterStr, strlen(defaultUpdaterStr) + 1);
        } else {
            memcpy(g_bootArgsStr, defaultRebootStr, strlen(defaultRebootStr) + 1);
        }
    } else {
        if (g_isRecovery) {
            memcpy(g_bootArgsStr, updaterHead, strlen(updaterHead) + 1);
        } else {
            memcpy(g_bootArgsStr, rebootHead, strlen(rebootHead) + 1);
        }
        memcpy(g_bootArgsStr + strlen(g_bootArgsStr), &block2[PARTITION_INFO_POS], partitionStrLen + 1);
    }
    printf("@@@ g_isRecovery = %d\n", g_isRecovery);
    printf("@@@ bootArgs from misc       = %s\n", g_bootArgsStr);

    return g_isRecovery;
}

int misc_init_r(void)
{
    const char cmdBuf[] = "mmc read 0x0 0x80000000 0x800 0x4800; bootm 0x80000000";

#ifdef CONFIG_RANDOM_ETHADDR
	random_init_r();
#endif
	env_set("verify", "n");

#if (CONFIG_AUTO_UPDATE == 1)
    if (EmmcInitParam() == -1) {
        return 0;
    }
    ChangeBootArgs();

    env_set("bootargs", g_bootArgsStr);
    env_set("bootcmd", cmdBuf);

    /* auto update flag */
    if (is_auto_update())
        auto_update_flag = 1;
    else
        auto_update_flag = 0;

    /* bare chip program flag */
    if (is_bare_program())
        bare_chip_program = 1;
    else
        bare_chip_program = 0;

#ifdef CFG_MMU_HANDLEOK
	dcache_stop();
#endif

#ifdef CFG_MMU_HANDLEOK
	dcache_start();
#endif

#endif

#if (CONFIG_AUTO_UPDATE == 1)
    int update_flag = -1;
    if (auto_update_flag)
        update_flag = do_auto_update();
    if (bare_chip_program && !auto_update_flag)
        save_bootdata_to_flash();
    if (update_flag == 0)
        do_reset(NULL, 0, 0, NULL);
#endif
    return 0;
}

int board_init(void)
{
	DECLARE_GLOBAL_DATA_PTR;

	gd->bd->bi_arch_number = MACH_TYPE_HI3516DV300;
	gd->bd->bi_boot_params = CFG_BOOT_PARAMS;

	boot_flag_init();

	return 0;
}

int dram_init(void)
{
	DECLARE_GLOBAL_DATA_PTR;

	gd->ram_size = PHYS_SDRAM_1_SIZE;
	return 0;
}

void reset_cpu(ulong addr)
{
	/* 0x12345678:writing any value will cause a reset. */
	writel(0x12345678, REG_BASE_SCTL + REG_SC_SYSRES);
	while (1);
}

int timer_init(void)
{
	/*
	 * Under uboot, 0xffffffff is set to load register,]
	 * timer_clk equals BUSCLK/2/256.
	 * e.g. BUSCLK equals 50M, it will roll back after 0xffffffff/timer_clk
	 * 43980s equals 12hours
	 */
	__raw_writel(0, CFG_TIMERBASE + REG_TIMER_CONTROL);
	__raw_writel(~0, CFG_TIMERBASE + REG_TIMER_RELOAD);

	/* 32 bit, periodic */
	__raw_writel(CFG_TIMER_CTRL, CFG_TIMERBASE + REG_TIMER_CONTROL);

	return 0;
}

int board_eth_init(bd_t *bis)
{
	int rc = 0;

#ifdef CONFIG_HISFV300_ETH
	rc = hieth_initialize(bis);
#endif
	return rc;
}


#ifdef CONFIG_GENERIC_MMC
int board_mmc_init(bd_t *bis)
{
	int ret = 0;
	int dev_num = 0;

#ifdef CONFIG_SUPPORT_EMMC_BOOT
	ret = himci_add_port(dev_num, EMMC_REG_BASE, CONFIG_HIMCI_MAX_FREQ);
	if (!ret) {
		ret = himci_probe(dev_num);
		if (ret)
			printf("No EMMC device found !\n");
	}
	dev_num++;
#endif

#ifdef CONFIG_AUTO_SD_UPDATE
#ifdef CONFIG_AUTO_OTA_UPDATE
	ret = himci_add_port(dev_num, SDIO0_REG_BASE, CONFIG_SDIO0_FREQ);
	if (!ret) {
		ret = himci_probe(dev_num);
		if (ret)
			printf("No SD device found !\n");
		else
			g_dev_num = dev_num;
	}
#else
	if (is_auto_update()) {
		ret = himci_add_port(dev_num, SDIO0_REG_BASE, CONFIG_SDIO0_FREQ);
		if (!ret) {
			ret = himci_probe(dev_num);
			if (ret)
				printf("No SD device found !\n");
		}
	}
#endif
#endif

	return ret;
}
#endif

int start_other_cpus(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	/* prepare two commands for cortex-a17 */
	volatile int cmd_address = 0;
	volatile int cmd = 0xe51ff004; /* 0xe51ff004:Default liteos address. */
	unsigned int regval;

	if (argc < NUM_2) {
		cmd_usage(cmdtp);
		return 1;
	}

	flush_dcache_all();
	asm("str %0, [%1]"::"r"(cmd), "r"(cmd_address): "cc");
	cmd = simple_strtoul(argv[NUM_1], NULL, 16); /* 16:base */
	printf("starting cpu1 liteos address 0x%x\n", cmd);
	asm("str %0, [%1, #4]"::"r"(cmd), "r"(cmd_address): "cc");
	/* clear the slave cpu reset */
	/* 0x12010000:CRG base address; 0x0078:PERI_CRG30 */
	regval = readl(0x12010000 + 0x0078);
	regval &= ~(NUM_1 << NUM_2);
	/* 0x12010000:CRG base address; 0x0078:PERI_CRG30 */
	writel(regval, (0x12010000 + 0x0078));
	return 0;
}

U_BOOT_CMD(
		go_cpu1, CONFIG_SYS_MAXARGS, 0, start_other_cpus,
		"Perform power on and unreset  CPU1_A7",
		"go_cpu1 <address>\n"
	  );

#ifdef CONFIG_ARMV7_NONSEC
void smp_set_core_boot_addr(unsigned long addr, int corenr)
{
}

void smp_kick_all_cpus(void)
{
}

void smp_waitloop(unsigned previous_address)
{
}
#endif
