// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware RNG bridge for NVIDIA Tegra186 SE-ELP RNG1
 *
 * Tegra186's SE-ELP driver exposes two crypto RNG algorithms.  NVIDIA's
 * downstream T186 hwrng driver used rng1-elp-tegra; the PKA1 raw TRNG is a
 * separate engine and depends on the PKA1 completion interrupt.  Keep the
 * hwrng path on RNG1, matching NVIDIA's working downstream implementation.
 */

#include <crypto/rng.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hw_random.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#define TEGRA_RNG_NAME		"tegra-rng"
#define TEGRA_RNG_ALG		"rng1-elp-tegra"
#define TEGRA_RNG_RETRY_MS	20
#define TEGRA_RNG_TIMEOUT_MS	2000

static int tegra_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	unsigned long deadline;
	struct crypto_rng *tfm;
	int ret;

	/* Match NVIDIA's downstream driver: do not block an atomic probe read. */
	if (!wait)
		return 0;

	tfm = crypto_alloc_rng(TEGRA_RNG_ALG, CRYPTO_ALG_TYPE_RNG, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		pr_err("tegra-rng: crypto_alloc_rng(%s) failed: %d\n",
		       TEGRA_RNG_ALG, ret);
		return ret;
	}

	deadline = jiffies + msecs_to_jiffies(TEGRA_RNG_TIMEOUT_MS);
	do {
		ret = crypto_rng_get_bytes(tfm, data, max);
		if (ret != -EAGAIN)
			break;

		if (msleep_interruptible(TEGRA_RNG_RETRY_MS)) {
			ret = -ERESTARTSYS;
			break;
		}
	} while (time_before_eq(jiffies, deadline));

	/* crypto_rng_get_bytes() returns zero on success. */
	if (!ret)
		ret = max;

	crypto_free_rng(tfm);
	return ret;
}

static struct hwrng tegra_rng = {
	.name = TEGRA_RNG_NAME,
	.read = tegra_rng_read,
};

static int __init tegra_rng_init(void)
{
	return hwrng_register(&tegra_rng);
}
module_init(tegra_rng_init);

static void __exit tegra_rng_exit(void)
{
	hwrng_unregister(&tegra_rng);
}
module_exit(tegra_rng_exit);

MODULE_DESCRIPTION("RNG driver for NVIDIA Tegra186 SE-ELP RNG1");
MODULE_AUTHOR("NVIDIA Corporation; Linux 5.10 Tegra186 port");
MODULE_LICENSE("GPL");
