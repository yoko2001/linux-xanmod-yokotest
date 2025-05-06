#ifndef _MM_MULTISWP_DBG_H_
#define _MM_MULTISWP_DBG_H_
#ifdef CONFIG_SWAP

#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_INFO(fmt, ...) \
    do {pr_info(pr_fmt(fmt), ##__VA_ARGS__);} while (0)
#else
#define MULTISWAP_MIG_INFO(fmt, ...) \
    do {} while (0)
#endif
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_INFO_ON(condition, fmt, ...) \
	do { if (condition) pr_info(pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#else
#define MULTISWAP_MIG_INFO_ON(condition, fmt, ...) \
	do {} while (0)
#endif

/* WARN */
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_WARN(fmt, ...) \
    do { pr_warn(pr_fmt(fmt), ##__VA_ARGS__);} while (0)
#else
#define MULTISWAP_MIG_WARN(fmt, ...) \
    do {} while (0)
#endif
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_WARN_ON(condition, fmt, ...) \
	do { if (condition) pr_warn(pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#else
#define MULTISWAP_MIG_WARN_ON(condition, fmt, ...) \
	do {} while (0)
#endif

/* ERR */
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_ERR(fmt, ...) \
    do { pr_err(pr_fmt(fmt), ##__VA_ARGS__);} while (0)
#else
#define MULTISWAP_MIG_ERR(fmt, ...) \
    do {} while (0)
#endif
#ifdef CONFIG_LRU_GEN_STALE_SWP_ENTRY_SAVIOR_DEBUG
#define MULTISWAP_MIG_ERR_ON(condition, fmt, ...) \
	do { if (condition) pr_err(pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#else
#define MULTISWAP_MIG_ERR_ON(condition, fmt, ...) \
	do {} while (0)
#endif
#endif /* CONFIG_SWAP */
#endif /* _MM_MULTISWP_DBG_H_ */
