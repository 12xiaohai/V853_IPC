#ifndef VIPP4_TEST_NATIVE_ISP_H
#define VIPP4_TEST_NATIVE_ISP_H
/* 模拟本SDK的33字节原生配置，非不匹配的MPI公共32位结构。 */
struct isp_test_enable_cfg {
    signed char manual, afs, ae, af, awb, hist, wdr_split, wdr_stitch;
    signed char otf_dpc, ctc, gca, nrp, denoise, tdf, blc, wb, dig_gain;
    signed char lsc, msc, pltm, cfa, lca, sharp, ccm, defog, cnr, drc;
    signed char gtm, gamma, cem, encpp_en, enc_3dnr_en, enc_2dnr_en;
};
enum { HW_ISP_CFG_TEST = 1, HW_ISP_CFG_TEST_ENABLE = 0x20 };
int isp_get_cfg(int, unsigned char, unsigned int, void *);
int isp_set_cfg(int, unsigned char, unsigned int, void *);
int isp_update(int);
#endif
