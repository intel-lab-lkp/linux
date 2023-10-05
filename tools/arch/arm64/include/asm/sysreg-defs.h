#ifndef __ASM_SYSREG_DEFS_H
#define __ASM_SYSREG_DEFS_H

/* Generated file - do not edit */

#define REG_OSDTRRX_EL1                                 S2_0_C0_C0_2
#define SYS_OSDTRRX_EL1                                 sys_reg(2, 0, 0, 0, 2)
#define SYS_OSDTRRX_EL1_Op0                             2
#define SYS_OSDTRRX_EL1_Op1                             0
#define SYS_OSDTRRX_EL1_CRn                             0
#define SYS_OSDTRRX_EL1_CRm                             0
#define SYS_OSDTRRX_EL1_Op2                             2

#define OSDTRRX_EL1_DTRRX                               GENMASK(31, 0)
#define OSDTRRX_EL1_DTRRX_MASK                          GENMASK(31, 0)
#define OSDTRRX_EL1_DTRRX_SHIFT                         0
#define OSDTRRX_EL1_DTRRX_WIDTH                         32

#define OSDTRRX_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 32))
#define OSDTRRX_EL1_RES1                                (UL(0))
#define OSDTRRX_EL1_UNKN                                (UL(0))

#define REG_MDCCINT_EL1                                 S2_0_C0_C2_0
#define SYS_MDCCINT_EL1                                 sys_reg(2, 0, 0, 2, 0)
#define SYS_MDCCINT_EL1_Op0                             2
#define SYS_MDCCINT_EL1_Op1                             0
#define SYS_MDCCINT_EL1_CRn                             0
#define SYS_MDCCINT_EL1_CRm                             2
#define SYS_MDCCINT_EL1_Op2                             0

#define MDCCINT_EL1_RX                                  GENMASK(30, 30)
#define MDCCINT_EL1_RX_MASK                             GENMASK(30, 30)
#define MDCCINT_EL1_RX_SHIFT                            30
#define MDCCINT_EL1_RX_WIDTH                            1

#define MDCCINT_EL1_TX                                  GENMASK(29, 29)
#define MDCCINT_EL1_TX_MASK                             GENMASK(29, 29)
#define MDCCINT_EL1_TX_SHIFT                            29
#define MDCCINT_EL1_TX_WIDTH                            1

#define MDCCINT_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 31) | GENMASK_ULL(28, 0))
#define MDCCINT_EL1_RES1                                (UL(0))
#define MDCCINT_EL1_UNKN                                (UL(0))

#define REG_MDSCR_EL1                                   S2_0_C0_C2_2
#define SYS_MDSCR_EL1                                   sys_reg(2, 0, 0, 2, 2)
#define SYS_MDSCR_EL1_Op0                               2
#define SYS_MDSCR_EL1_Op1                               0
#define SYS_MDSCR_EL1_CRn                               0
#define SYS_MDSCR_EL1_CRm                               2
#define SYS_MDSCR_EL1_Op2                               2

#define MDSCR_EL1_EHBWE                                 GENMASK(35, 35)
#define MDSCR_EL1_EHBWE_MASK                            GENMASK(35, 35)
#define MDSCR_EL1_EHBWE_SHIFT                           35
#define MDSCR_EL1_EHBWE_WIDTH                           1

#define MDSCR_EL1_EnSPM                                 GENMASK(34, 34)
#define MDSCR_EL1_EnSPM_MASK                            GENMASK(34, 34)
#define MDSCR_EL1_EnSPM_SHIFT                           34
#define MDSCR_EL1_EnSPM_WIDTH                           1

#define MDSCR_EL1_TTA                                   GENMASK(33, 33)
#define MDSCR_EL1_TTA_MASK                              GENMASK(33, 33)
#define MDSCR_EL1_TTA_SHIFT                             33
#define MDSCR_EL1_TTA_WIDTH                             1

#define MDSCR_EL1_EMBWE                                 GENMASK(32, 32)
#define MDSCR_EL1_EMBWE_MASK                            GENMASK(32, 32)
#define MDSCR_EL1_EMBWE_SHIFT                           32
#define MDSCR_EL1_EMBWE_WIDTH                           1

#define MDSCR_EL1_TFO                                   GENMASK(31, 31)
#define MDSCR_EL1_TFO_MASK                              GENMASK(31, 31)
#define MDSCR_EL1_TFO_SHIFT                             31
#define MDSCR_EL1_TFO_WIDTH                             1

#define MDSCR_EL1_RXfull                                GENMASK(30, 30)
#define MDSCR_EL1_RXfull_MASK                           GENMASK(30, 30)
#define MDSCR_EL1_RXfull_SHIFT                          30
#define MDSCR_EL1_RXfull_WIDTH                          1

#define MDSCR_EL1_TXfull                                GENMASK(29, 29)
#define MDSCR_EL1_TXfull_MASK                           GENMASK(29, 29)
#define MDSCR_EL1_TXfull_SHIFT                          29
#define MDSCR_EL1_TXfull_WIDTH                          1

#define MDSCR_EL1_RXO                                   GENMASK(27, 27)
#define MDSCR_EL1_RXO_MASK                              GENMASK(27, 27)
#define MDSCR_EL1_RXO_SHIFT                             27
#define MDSCR_EL1_RXO_WIDTH                             1

#define MDSCR_EL1_TXU                                   GENMASK(26, 26)
#define MDSCR_EL1_TXU_MASK                              GENMASK(26, 26)
#define MDSCR_EL1_TXU_SHIFT                             26
#define MDSCR_EL1_TXU_WIDTH                             1

#define MDSCR_EL1_INTdis                                GENMASK(23, 22)
#define MDSCR_EL1_INTdis_MASK                           GENMASK(23, 22)
#define MDSCR_EL1_INTdis_SHIFT                          22
#define MDSCR_EL1_INTdis_WIDTH                          2

#define MDSCR_EL1_TDA                                   GENMASK(21, 21)
#define MDSCR_EL1_TDA_MASK                              GENMASK(21, 21)
#define MDSCR_EL1_TDA_SHIFT                             21
#define MDSCR_EL1_TDA_WIDTH                             1

#define MDSCR_EL1_SC2                                   GENMASK(19, 19)
#define MDSCR_EL1_SC2_MASK                              GENMASK(19, 19)
#define MDSCR_EL1_SC2_SHIFT                             19
#define MDSCR_EL1_SC2_WIDTH                             1

#define MDSCR_EL1_MDE                                   GENMASK(15, 15)
#define MDSCR_EL1_MDE_MASK                              GENMASK(15, 15)
#define MDSCR_EL1_MDE_SHIFT                             15
#define MDSCR_EL1_MDE_WIDTH                             1

#define MDSCR_EL1_HDE                                   GENMASK(14, 14)
#define MDSCR_EL1_HDE_MASK                              GENMASK(14, 14)
#define MDSCR_EL1_HDE_SHIFT                             14
#define MDSCR_EL1_HDE_WIDTH                             1

#define MDSCR_EL1_KDE                                   GENMASK(13, 13)
#define MDSCR_EL1_KDE_MASK                              GENMASK(13, 13)
#define MDSCR_EL1_KDE_SHIFT                             13
#define MDSCR_EL1_KDE_WIDTH                             1

#define MDSCR_EL1_TDCC                                  GENMASK(12, 12)
#define MDSCR_EL1_TDCC_MASK                             GENMASK(12, 12)
#define MDSCR_EL1_TDCC_SHIFT                            12
#define MDSCR_EL1_TDCC_WIDTH                            1

#define MDSCR_EL1_ERR                                   GENMASK(6, 6)
#define MDSCR_EL1_ERR_MASK                              GENMASK(6, 6)
#define MDSCR_EL1_ERR_SHIFT                             6
#define MDSCR_EL1_ERR_WIDTH                             1

#define MDSCR_EL1_SS                                    GENMASK(0, 0)
#define MDSCR_EL1_SS_MASK                               GENMASK(0, 0)
#define MDSCR_EL1_SS_SHIFT                              0
#define MDSCR_EL1_SS_WIDTH                              1

#define MDSCR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 36) | GENMASK_ULL(28, 28) | GENMASK_ULL(25, 24) | GENMASK_ULL(20, 20) | GENMASK_ULL(18, 16) | GENMASK_ULL(11, 7) | GENMASK_ULL(5, 1))
#define MDSCR_EL1_RES1                                  (UL(0))
#define MDSCR_EL1_UNKN                                  (UL(0))

#define REG_OSDTRTX_EL1                                 S2_0_C0_C3_2
#define SYS_OSDTRTX_EL1                                 sys_reg(2, 0, 0, 3, 2)
#define SYS_OSDTRTX_EL1_Op0                             2
#define SYS_OSDTRTX_EL1_Op1                             0
#define SYS_OSDTRTX_EL1_CRn                             0
#define SYS_OSDTRTX_EL1_CRm                             3
#define SYS_OSDTRTX_EL1_Op2                             2

#define OSDTRTX_EL1_DTRTX                               GENMASK(31, 0)
#define OSDTRTX_EL1_DTRTX_MASK                          GENMASK(31, 0)
#define OSDTRTX_EL1_DTRTX_SHIFT                         0
#define OSDTRTX_EL1_DTRTX_WIDTH                         32

#define OSDTRTX_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 32))
#define OSDTRTX_EL1_RES1                                (UL(0))
#define OSDTRTX_EL1_UNKN                                (UL(0))

#define REG_OSECCR_EL1                                  S2_0_C0_C6_2
#define SYS_OSECCR_EL1                                  sys_reg(2, 0, 0, 6, 2)
#define SYS_OSECCR_EL1_Op0                              2
#define SYS_OSECCR_EL1_Op1                              0
#define SYS_OSECCR_EL1_CRn                              0
#define SYS_OSECCR_EL1_CRm                              6
#define SYS_OSECCR_EL1_Op2                              2

#define OSECCR_EL1_EDECCR                               GENMASK(31, 0)
#define OSECCR_EL1_EDECCR_MASK                          GENMASK(31, 0)
#define OSECCR_EL1_EDECCR_SHIFT                         0
#define OSECCR_EL1_EDECCR_WIDTH                         32

#define OSECCR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 32))
#define OSECCR_EL1_RES1                                 (UL(0))
#define OSECCR_EL1_UNKN                                 (UL(0))

#define REG_OSLAR_EL1                                   S2_0_C1_C0_4
#define SYS_OSLAR_EL1                                   sys_reg(2, 0, 1, 0, 4)
#define SYS_OSLAR_EL1_Op0                               2
#define SYS_OSLAR_EL1_Op1                               0
#define SYS_OSLAR_EL1_CRn                               1
#define SYS_OSLAR_EL1_CRm                               0
#define SYS_OSLAR_EL1_Op2                               4

#define OSLAR_EL1_OSLK                                  GENMASK(0, 0)
#define OSLAR_EL1_OSLK_MASK                             GENMASK(0, 0)
#define OSLAR_EL1_OSLK_SHIFT                            0
#define OSLAR_EL1_OSLK_WIDTH                            1

#define OSLAR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 1))
#define OSLAR_EL1_RES1                                  (UL(0))
#define OSLAR_EL1_UNKN                                  (UL(0))

#define REG_ID_PFR0_EL1                                 S3_0_C0_C1_0
#define SYS_ID_PFR0_EL1                                 sys_reg(3, 0, 0, 1, 0)
#define SYS_ID_PFR0_EL1_Op0                             3
#define SYS_ID_PFR0_EL1_Op1                             0
#define SYS_ID_PFR0_EL1_CRn                             0
#define SYS_ID_PFR0_EL1_CRm                             1
#define SYS_ID_PFR0_EL1_Op2                             0

#define ID_PFR0_EL1_RAS                                 GENMASK(31, 28)
#define ID_PFR0_EL1_RAS_MASK                            GENMASK(31, 28)
#define ID_PFR0_EL1_RAS_SHIFT                           28
#define ID_PFR0_EL1_RAS_WIDTH                           4
#define ID_PFR0_EL1_RAS_SIGNED                          false
#define ID_PFR0_EL1_RAS_NI                              UL(0b0000)
#define ID_PFR0_EL1_RAS_RAS                             UL(0b0001)
#define ID_PFR0_EL1_RAS_RASv1p1                         UL(0b0010)

#define ID_PFR0_EL1_DIT                                 GENMASK(27, 24)
#define ID_PFR0_EL1_DIT_MASK                            GENMASK(27, 24)
#define ID_PFR0_EL1_DIT_SHIFT                           24
#define ID_PFR0_EL1_DIT_WIDTH                           4
#define ID_PFR0_EL1_DIT_SIGNED                          false
#define ID_PFR0_EL1_DIT_NI                              UL(0b0000)
#define ID_PFR0_EL1_DIT_IMP                             UL(0b0001)

#define ID_PFR0_EL1_AMU                                 GENMASK(23, 20)
#define ID_PFR0_EL1_AMU_MASK                            GENMASK(23, 20)
#define ID_PFR0_EL1_AMU_SHIFT                           20
#define ID_PFR0_EL1_AMU_WIDTH                           4
#define ID_PFR0_EL1_AMU_SIGNED                          false
#define ID_PFR0_EL1_AMU_NI                              UL(0b0000)
#define ID_PFR0_EL1_AMU_AMUv1                           UL(0b0001)
#define ID_PFR0_EL1_AMU_AMUv1p1                         UL(0b0010)

#define ID_PFR0_EL1_CSV2                                GENMASK(19, 16)
#define ID_PFR0_EL1_CSV2_MASK                           GENMASK(19, 16)
#define ID_PFR0_EL1_CSV2_SHIFT                          16
#define ID_PFR0_EL1_CSV2_WIDTH                          4
#define ID_PFR0_EL1_CSV2_SIGNED                         false
#define ID_PFR0_EL1_CSV2_UNDISCLOSED                    UL(0b0000)
#define ID_PFR0_EL1_CSV2_IMP                            UL(0b0001)
#define ID_PFR0_EL1_CSV2_CSV2p1                         UL(0b0010)

#define ID_PFR0_EL1_State3                              GENMASK(15, 12)
#define ID_PFR0_EL1_State3_MASK                         GENMASK(15, 12)
#define ID_PFR0_EL1_State3_SHIFT                        12
#define ID_PFR0_EL1_State3_WIDTH                        4
#define ID_PFR0_EL1_State3_SIGNED                       false
#define ID_PFR0_EL1_State3_NI                           UL(0b0000)
#define ID_PFR0_EL1_State3_IMP                          UL(0b0001)

#define ID_PFR0_EL1_State2                              GENMASK(11, 8)
#define ID_PFR0_EL1_State2_MASK                         GENMASK(11, 8)
#define ID_PFR0_EL1_State2_SHIFT                        8
#define ID_PFR0_EL1_State2_WIDTH                        4
#define ID_PFR0_EL1_State2_NI                           UL(0b0000)
#define ID_PFR0_EL1_State2_NO_CV                        UL(0b0001)
#define ID_PFR0_EL1_State2_CV                           UL(0b0010)

#define ID_PFR0_EL1_State1                              GENMASK(7, 4)
#define ID_PFR0_EL1_State1_MASK                         GENMASK(7, 4)
#define ID_PFR0_EL1_State1_SHIFT                        4
#define ID_PFR0_EL1_State1_WIDTH                        4
#define ID_PFR0_EL1_State1_SIGNED                       false
#define ID_PFR0_EL1_State1_NI                           UL(0b0000)
#define ID_PFR0_EL1_State1_THUMB                        UL(0b0001)
#define ID_PFR0_EL1_State1_THUMB2                       UL(0b0010)

#define ID_PFR0_EL1_State0                              GENMASK(3, 0)
#define ID_PFR0_EL1_State0_MASK                         GENMASK(3, 0)
#define ID_PFR0_EL1_State0_SHIFT                        0
#define ID_PFR0_EL1_State0_WIDTH                        4
#define ID_PFR0_EL1_State0_SIGNED                       false
#define ID_PFR0_EL1_State0_NI                           UL(0b0000)
#define ID_PFR0_EL1_State0_IMP                          UL(0b0001)

#define ID_PFR0_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 32))
#define ID_PFR0_EL1_RES1                                (UL(0))
#define ID_PFR0_EL1_UNKN                                (UL(0))

#define REG_ID_PFR1_EL1                                 S3_0_C0_C1_1
#define SYS_ID_PFR1_EL1                                 sys_reg(3, 0, 0, 1, 1)
#define SYS_ID_PFR1_EL1_Op0                             3
#define SYS_ID_PFR1_EL1_Op1                             0
#define SYS_ID_PFR1_EL1_CRn                             0
#define SYS_ID_PFR1_EL1_CRm                             1
#define SYS_ID_PFR1_EL1_Op2                             1

#define ID_PFR1_EL1_GIC                                 GENMASK(31, 28)
#define ID_PFR1_EL1_GIC_MASK                            GENMASK(31, 28)
#define ID_PFR1_EL1_GIC_SHIFT                           28
#define ID_PFR1_EL1_GIC_WIDTH                           4
#define ID_PFR1_EL1_GIC_SIGNED                          false
#define ID_PFR1_EL1_GIC_NI                              UL(0b0000)
#define ID_PFR1_EL1_GIC_GICv3                           UL(0b0001)
#define ID_PFR1_EL1_GIC_GICv4p1                         UL(0b0010)

#define ID_PFR1_EL1_Virt_frac                           GENMASK(27, 24)
#define ID_PFR1_EL1_Virt_frac_MASK                      GENMASK(27, 24)
#define ID_PFR1_EL1_Virt_frac_SHIFT                     24
#define ID_PFR1_EL1_Virt_frac_WIDTH                     4
#define ID_PFR1_EL1_Virt_frac_SIGNED                    false
#define ID_PFR1_EL1_Virt_frac_NI                        UL(0b0000)
#define ID_PFR1_EL1_Virt_frac_IMP                       UL(0b0001)

#define ID_PFR1_EL1_Sec_frac                            GENMASK(23, 20)
#define ID_PFR1_EL1_Sec_frac_MASK                       GENMASK(23, 20)
#define ID_PFR1_EL1_Sec_frac_SHIFT                      20
#define ID_PFR1_EL1_Sec_frac_WIDTH                      4
#define ID_PFR1_EL1_Sec_frac_NI                         UL(0b0000)
#define ID_PFR1_EL1_Sec_frac_WALK_DISABLE               UL(0b0001)
#define ID_PFR1_EL1_Sec_frac_SECURE_MEMORY              UL(0b0010)

#define ID_PFR1_EL1_GenTimer                            GENMASK(19, 16)
#define ID_PFR1_EL1_GenTimer_MASK                       GENMASK(19, 16)
#define ID_PFR1_EL1_GenTimer_SHIFT                      16
#define ID_PFR1_EL1_GenTimer_WIDTH                      4
#define ID_PFR1_EL1_GenTimer_SIGNED                     false
#define ID_PFR1_EL1_GenTimer_NI                         UL(0b0000)
#define ID_PFR1_EL1_GenTimer_IMP                        UL(0b0001)
#define ID_PFR1_EL1_GenTimer_ECV                        UL(0b0010)

#define ID_PFR1_EL1_Virtualization                      GENMASK(15, 12)
#define ID_PFR1_EL1_Virtualization_MASK                 GENMASK(15, 12)
#define ID_PFR1_EL1_Virtualization_SHIFT                12
#define ID_PFR1_EL1_Virtualization_WIDTH                4
#define ID_PFR1_EL1_Virtualization_SIGNED               false
#define ID_PFR1_EL1_Virtualization_NI                   UL(0b0000)
#define ID_PFR1_EL1_Virtualization_IMP                  UL(0b0001)

#define ID_PFR1_EL1_MProgMod                            GENMASK(11, 8)
#define ID_PFR1_EL1_MProgMod_MASK                       GENMASK(11, 8)
#define ID_PFR1_EL1_MProgMod_SHIFT                      8
#define ID_PFR1_EL1_MProgMod_WIDTH                      4
#define ID_PFR1_EL1_MProgMod_SIGNED                     false
#define ID_PFR1_EL1_MProgMod_NI                         UL(0b0000)
#define ID_PFR1_EL1_MProgMod_IMP                        UL(0b0001)

#define ID_PFR1_EL1_Security                            GENMASK(7, 4)
#define ID_PFR1_EL1_Security_MASK                       GENMASK(7, 4)
#define ID_PFR1_EL1_Security_SHIFT                      4
#define ID_PFR1_EL1_Security_WIDTH                      4
#define ID_PFR1_EL1_Security_NI                         UL(0b0000)
#define ID_PFR1_EL1_Security_EL3                        UL(0b0001)
#define ID_PFR1_EL1_Security_NSACR_RFR                  UL(0b0001)

#define ID_PFR1_EL1_ProgMod                             GENMASK(3, 0)
#define ID_PFR1_EL1_ProgMod_MASK                        GENMASK(3, 0)
#define ID_PFR1_EL1_ProgMod_SHIFT                       0
#define ID_PFR1_EL1_ProgMod_WIDTH                       4
#define ID_PFR1_EL1_ProgMod_SIGNED                      false
#define ID_PFR1_EL1_ProgMod_NI                          UL(0b0000)
#define ID_PFR1_EL1_ProgMod_IMP                         UL(0b0001)

#define ID_PFR1_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 32))
#define ID_PFR1_EL1_RES1                                (UL(0))
#define ID_PFR1_EL1_UNKN                                (UL(0))

#define REG_ID_DFR0_EL1                                 S3_0_C0_C1_2
#define SYS_ID_DFR0_EL1                                 sys_reg(3, 0, 0, 1, 2)
#define SYS_ID_DFR0_EL1_Op0                             3
#define SYS_ID_DFR0_EL1_Op1                             0
#define SYS_ID_DFR0_EL1_CRn                             0
#define SYS_ID_DFR0_EL1_CRm                             1
#define SYS_ID_DFR0_EL1_Op2                             2

#define ID_DFR0_EL1_TraceFilt                           GENMASK(31, 28)
#define ID_DFR0_EL1_TraceFilt_MASK                      GENMASK(31, 28)
#define ID_DFR0_EL1_TraceFilt_SHIFT                     28
#define ID_DFR0_EL1_TraceFilt_WIDTH                     4
#define ID_DFR0_EL1_TraceFilt_SIGNED                    false
#define ID_DFR0_EL1_TraceFilt_NI                        UL(0b0000)
#define ID_DFR0_EL1_TraceFilt_IMP                       UL(0b0001)

#define ID_DFR0_EL1_PerfMon                             GENMASK(27, 24)
#define ID_DFR0_EL1_PerfMon_MASK                        GENMASK(27, 24)
#define ID_DFR0_EL1_PerfMon_SHIFT                       24
#define ID_DFR0_EL1_PerfMon_WIDTH                       4
#define ID_DFR0_EL1_PerfMon_SIGNED                      false
#define ID_DFR0_EL1_PerfMon_NI                          UL(0b0000)
#define ID_DFR0_EL1_PerfMon_PMUv1                       UL(0b0001)
#define ID_DFR0_EL1_PerfMon_PMUv2                       UL(0b0010)
#define ID_DFR0_EL1_PerfMon_PMUv3                       UL(0b0011)
#define ID_DFR0_EL1_PerfMon_PMUv3p1                     UL(0b0100)
#define ID_DFR0_EL1_PerfMon_PMUv3p4                     UL(0b0101)
#define ID_DFR0_EL1_PerfMon_PMUv3p5                     UL(0b0110)
#define ID_DFR0_EL1_PerfMon_PMUv3p7                     UL(0b0111)
#define ID_DFR0_EL1_PerfMon_PMUv3p8                     UL(0b1000)
#define ID_DFR0_EL1_PerfMon_IMPDEF                      UL(0b1111)

#define ID_DFR0_EL1_MProfDbg                            GENMASK(23, 20)
#define ID_DFR0_EL1_MProfDbg_MASK                       GENMASK(23, 20)
#define ID_DFR0_EL1_MProfDbg_SHIFT                      20
#define ID_DFR0_EL1_MProfDbg_WIDTH                      4
#define ID_DFR0_EL1_MProfDbg_NI                         UL(0b0000)
#define ID_DFR0_EL1_MProfDbg_IMP                        UL(0b0001)

#define ID_DFR0_EL1_MMapTrc                             GENMASK(19, 16)
#define ID_DFR0_EL1_MMapTrc_MASK                        GENMASK(19, 16)
#define ID_DFR0_EL1_MMapTrc_SHIFT                       16
#define ID_DFR0_EL1_MMapTrc_WIDTH                       4
#define ID_DFR0_EL1_MMapTrc_NI                          UL(0b0000)
#define ID_DFR0_EL1_MMapTrc_IMP                         UL(0b0001)

#define ID_DFR0_EL1_CopTrc                              GENMASK(15, 12)
#define ID_DFR0_EL1_CopTrc_MASK                         GENMASK(15, 12)
#define ID_DFR0_EL1_CopTrc_SHIFT                        12
#define ID_DFR0_EL1_CopTrc_WIDTH                        4
#define ID_DFR0_EL1_CopTrc_NI                           UL(0b0000)
#define ID_DFR0_EL1_CopTrc_IMP                          UL(0b0001)

#define ID_DFR0_EL1_MMapDbg                             GENMASK(11, 8)
#define ID_DFR0_EL1_MMapDbg_MASK                        GENMASK(11, 8)
#define ID_DFR0_EL1_MMapDbg_SHIFT                       8
#define ID_DFR0_EL1_MMapDbg_WIDTH                       4
#define ID_DFR0_EL1_MMapDbg_NI                          UL(0b0000)
#define ID_DFR0_EL1_MMapDbg_Armv7                       UL(0b0100)
#define ID_DFR0_EL1_MMapDbg_Armv7p1                     UL(0b0101)

#define ID_DFR0_EL1_CopSDbg                             GENMASK(7, 4)
#define ID_DFR0_EL1_CopSDbg_MASK                        GENMASK(7, 4)
#define ID_DFR0_EL1_CopSDbg_SHIFT                       4
#define ID_DFR0_EL1_CopSDbg_WIDTH                       4

#define ID_DFR0_EL1_CopDbg                              GENMASK(3, 0)
#define ID_DFR0_EL1_CopDbg_MASK                         GENMASK(3, 0)
#define ID_DFR0_EL1_CopDbg_SHIFT                        0
#define ID_DFR0_EL1_CopDbg_WIDTH                        4
#define ID_DFR0_EL1_CopDbg_NI                           UL(0b0000)
#define ID_DFR0_EL1_CopDbg_Armv6                        UL(0b0010)
#define ID_DFR0_EL1_CopDbg_Armv6p1                      UL(0b0011)
#define ID_DFR0_EL1_CopDbg_Armv7                        UL(0b0100)
#define ID_DFR0_EL1_CopDbg_Armv7p1                      UL(0b0101)
#define ID_DFR0_EL1_CopDbg_Armv8                        UL(0b0110)
#define ID_DFR0_EL1_CopDbg_VHE                          UL(0b0111)
#define ID_DFR0_EL1_CopDbg_Debugv8p2                    UL(0b1000)
#define ID_DFR0_EL1_CopDbg_Debugv8p4                    UL(0b1001)
#define ID_DFR0_EL1_CopDbg_Debugv8p8                    UL(0b1010)

#define ID_DFR0_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 32))
#define ID_DFR0_EL1_RES1                                (UL(0))
#define ID_DFR0_EL1_UNKN                                (UL(0))

#define REG_ID_AFR0_EL1                                 S3_0_C0_C1_3
#define SYS_ID_AFR0_EL1                                 sys_reg(3, 0, 0, 1, 3)
#define SYS_ID_AFR0_EL1_Op0                             3
#define SYS_ID_AFR0_EL1_Op1                             0
#define SYS_ID_AFR0_EL1_CRn                             0
#define SYS_ID_AFR0_EL1_CRm                             1
#define SYS_ID_AFR0_EL1_Op2                             3

#define ID_AFR0_EL1_IMPDEF3                             GENMASK(15, 12)
#define ID_AFR0_EL1_IMPDEF3_MASK                        GENMASK(15, 12)
#define ID_AFR0_EL1_IMPDEF3_SHIFT                       12
#define ID_AFR0_EL1_IMPDEF3_WIDTH                       4

#define ID_AFR0_EL1_IMPDEF2                             GENMASK(11, 8)
#define ID_AFR0_EL1_IMPDEF2_MASK                        GENMASK(11, 8)
#define ID_AFR0_EL1_IMPDEF2_SHIFT                       8
#define ID_AFR0_EL1_IMPDEF2_WIDTH                       4

#define ID_AFR0_EL1_IMPDEF1                             GENMASK(7, 4)
#define ID_AFR0_EL1_IMPDEF1_MASK                        GENMASK(7, 4)
#define ID_AFR0_EL1_IMPDEF1_SHIFT                       4
#define ID_AFR0_EL1_IMPDEF1_WIDTH                       4

#define ID_AFR0_EL1_IMPDEF0                             GENMASK(3, 0)
#define ID_AFR0_EL1_IMPDEF0_MASK                        GENMASK(3, 0)
#define ID_AFR0_EL1_IMPDEF0_SHIFT                       0
#define ID_AFR0_EL1_IMPDEF0_WIDTH                       4

#define ID_AFR0_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 16))
#define ID_AFR0_EL1_RES1                                (UL(0))
#define ID_AFR0_EL1_UNKN                                (UL(0))

#define REG_ID_MMFR0_EL1                                S3_0_C0_C1_4
#define SYS_ID_MMFR0_EL1                                sys_reg(3, 0, 0, 1, 4)
#define SYS_ID_MMFR0_EL1_Op0                            3
#define SYS_ID_MMFR0_EL1_Op1                            0
#define SYS_ID_MMFR0_EL1_CRn                            0
#define SYS_ID_MMFR0_EL1_CRm                            1
#define SYS_ID_MMFR0_EL1_Op2                            4

#define ID_MMFR0_EL1_InnerShr                           GENMASK(31, 28)
#define ID_MMFR0_EL1_InnerShr_MASK                      GENMASK(31, 28)
#define ID_MMFR0_EL1_InnerShr_SHIFT                     28
#define ID_MMFR0_EL1_InnerShr_WIDTH                     4
#define ID_MMFR0_EL1_InnerShr_NC                        UL(0b0000)
#define ID_MMFR0_EL1_InnerShr_HW                        UL(0b0001)
#define ID_MMFR0_EL1_InnerShr_IGNORED                   UL(0b1111)

#define ID_MMFR0_EL1_FCSE                               GENMASK(27, 24)
#define ID_MMFR0_EL1_FCSE_MASK                          GENMASK(27, 24)
#define ID_MMFR0_EL1_FCSE_SHIFT                         24
#define ID_MMFR0_EL1_FCSE_WIDTH                         4
#define ID_MMFR0_EL1_FCSE_SIGNED                        false
#define ID_MMFR0_EL1_FCSE_NI                            UL(0b0000)
#define ID_MMFR0_EL1_FCSE_IMP                           UL(0b0001)

#define ID_MMFR0_EL1_AuxReg                             GENMASK(23, 20)
#define ID_MMFR0_EL1_AuxReg_MASK                        GENMASK(23, 20)
#define ID_MMFR0_EL1_AuxReg_SHIFT                       20
#define ID_MMFR0_EL1_AuxReg_WIDTH                       4
#define ID_MMFR0_EL1_AuxReg_NI                          UL(0b0000)
#define ID_MMFR0_EL1_AuxReg_ACTLR                       UL(0b0001)
#define ID_MMFR0_EL1_AuxReg_AIFSR                       UL(0b0010)

#define ID_MMFR0_EL1_TCM                                GENMASK(19, 16)
#define ID_MMFR0_EL1_TCM_MASK                           GENMASK(19, 16)
#define ID_MMFR0_EL1_TCM_SHIFT                          16
#define ID_MMFR0_EL1_TCM_WIDTH                          4
#define ID_MMFR0_EL1_TCM_NI                             UL(0b0000)
#define ID_MMFR0_EL1_TCM_IMPDEF                         UL(0b0001)
#define ID_MMFR0_EL1_TCM_TCM                            UL(0b0010)
#define ID_MMFR0_EL1_TCM_TCM_DMA                        UL(0b0011)

#define ID_MMFR0_EL1_ShareLvl                           GENMASK(15, 12)
#define ID_MMFR0_EL1_ShareLvl_MASK                      GENMASK(15, 12)
#define ID_MMFR0_EL1_ShareLvl_SHIFT                     12
#define ID_MMFR0_EL1_ShareLvl_WIDTH                     4
#define ID_MMFR0_EL1_ShareLvl_ONE                       UL(0b0000)
#define ID_MMFR0_EL1_ShareLvl_TWO                       UL(0b0001)

#define ID_MMFR0_EL1_OuterShr                           GENMASK(11, 8)
#define ID_MMFR0_EL1_OuterShr_MASK                      GENMASK(11, 8)
#define ID_MMFR0_EL1_OuterShr_SHIFT                     8
#define ID_MMFR0_EL1_OuterShr_WIDTH                     4
#define ID_MMFR0_EL1_OuterShr_NC                        UL(0b0000)
#define ID_MMFR0_EL1_OuterShr_HW                        UL(0b0001)
#define ID_MMFR0_EL1_OuterShr_IGNORED                   UL(0b1111)

#define ID_MMFR0_EL1_PMSA                               GENMASK(7, 4)
#define ID_MMFR0_EL1_PMSA_MASK                          GENMASK(7, 4)
#define ID_MMFR0_EL1_PMSA_SHIFT                         4
#define ID_MMFR0_EL1_PMSA_WIDTH                         4
#define ID_MMFR0_EL1_PMSA_NI                            UL(0b0000)
#define ID_MMFR0_EL1_PMSA_IMPDEF                        UL(0b0001)
#define ID_MMFR0_EL1_PMSA_PMSAv6                        UL(0b0010)
#define ID_MMFR0_EL1_PMSA_PMSAv7                        UL(0b0011)

#define ID_MMFR0_EL1_VMSA                               GENMASK(3, 0)
#define ID_MMFR0_EL1_VMSA_MASK                          GENMASK(3, 0)
#define ID_MMFR0_EL1_VMSA_SHIFT                         0
#define ID_MMFR0_EL1_VMSA_WIDTH                         4
#define ID_MMFR0_EL1_VMSA_NI                            UL(0b0000)
#define ID_MMFR0_EL1_VMSA_IMPDEF                        UL(0b0001)
#define ID_MMFR0_EL1_VMSA_VMSAv6                        UL(0b0010)
#define ID_MMFR0_EL1_VMSA_VMSAv7                        UL(0b0011)
#define ID_MMFR0_EL1_VMSA_VMSAv7_PXN                    UL(0b0100)
#define ID_MMFR0_EL1_VMSA_VMSAv7_LONG                   UL(0b0101)

#define ID_MMFR0_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_MMFR0_EL1_RES1                               (UL(0))
#define ID_MMFR0_EL1_UNKN                               (UL(0))

#define REG_ID_MMFR1_EL1                                S3_0_C0_C1_5
#define SYS_ID_MMFR1_EL1                                sys_reg(3, 0, 0, 1, 5)
#define SYS_ID_MMFR1_EL1_Op0                            3
#define SYS_ID_MMFR1_EL1_Op1                            0
#define SYS_ID_MMFR1_EL1_CRn                            0
#define SYS_ID_MMFR1_EL1_CRm                            1
#define SYS_ID_MMFR1_EL1_Op2                            5

#define ID_MMFR1_EL1_BPred                              GENMASK(31, 28)
#define ID_MMFR1_EL1_BPred_MASK                         GENMASK(31, 28)
#define ID_MMFR1_EL1_BPred_SHIFT                        28
#define ID_MMFR1_EL1_BPred_WIDTH                        4
#define ID_MMFR1_EL1_BPred_NI                           UL(0b0000)
#define ID_MMFR1_EL1_BPred_BP_SW_MANGED                 UL(0b0001)
#define ID_MMFR1_EL1_BPred_BP_ASID_AWARE                UL(0b0010)
#define ID_MMFR1_EL1_BPred_BP_NOSNOOP                   UL(0b0011)
#define ID_MMFR1_EL1_BPred_BP_INVISIBLE                 UL(0b0100)

#define ID_MMFR1_EL1_L1TstCln                           GENMASK(27, 24)
#define ID_MMFR1_EL1_L1TstCln_MASK                      GENMASK(27, 24)
#define ID_MMFR1_EL1_L1TstCln_SHIFT                     24
#define ID_MMFR1_EL1_L1TstCln_WIDTH                     4
#define ID_MMFR1_EL1_L1TstCln_NI                        UL(0b0000)
#define ID_MMFR1_EL1_L1TstCln_NOINVALIDATE              UL(0b0001)
#define ID_MMFR1_EL1_L1TstCln_INVALIDATE                UL(0b0010)

#define ID_MMFR1_EL1_L1Uni                              GENMASK(23, 20)
#define ID_MMFR1_EL1_L1Uni_MASK                         GENMASK(23, 20)
#define ID_MMFR1_EL1_L1Uni_SHIFT                        20
#define ID_MMFR1_EL1_L1Uni_WIDTH                        4
#define ID_MMFR1_EL1_L1Uni_NI                           UL(0b0000)
#define ID_MMFR1_EL1_L1Uni_INVALIDATE                   UL(0b0001)
#define ID_MMFR1_EL1_L1Uni_CLEAN_AND_INVALIDATE         UL(0b0010)

#define ID_MMFR1_EL1_L1Hvd                              GENMASK(19, 16)
#define ID_MMFR1_EL1_L1Hvd_MASK                         GENMASK(19, 16)
#define ID_MMFR1_EL1_L1Hvd_SHIFT                        16
#define ID_MMFR1_EL1_L1Hvd_WIDTH                        4
#define ID_MMFR1_EL1_L1Hvd_NI                           UL(0b0000)
#define ID_MMFR1_EL1_L1Hvd_INVALIDATE_ISIDE_ONLY        UL(0b0001)
#define ID_MMFR1_EL1_L1Hvd_INVALIDATE                   UL(0b0010)
#define ID_MMFR1_EL1_L1Hvd_CLEAN_AND_INVALIDATE         UL(0b0011)

#define ID_MMFR1_EL1_L1UniSW                            GENMASK(15, 12)
#define ID_MMFR1_EL1_L1UniSW_MASK                       GENMASK(15, 12)
#define ID_MMFR1_EL1_L1UniSW_SHIFT                      12
#define ID_MMFR1_EL1_L1UniSW_WIDTH                      4
#define ID_MMFR1_EL1_L1UniSW_NI                         UL(0b0000)
#define ID_MMFR1_EL1_L1UniSW_CLEAN                      UL(0b0001)
#define ID_MMFR1_EL1_L1UniSW_CLEAN_AND_INVALIDATE       UL(0b0010)
#define ID_MMFR1_EL1_L1UniSW_INVALIDATE                 UL(0b0011)

#define ID_MMFR1_EL1_L1HvdSW                            GENMASK(11, 8)
#define ID_MMFR1_EL1_L1HvdSW_MASK                       GENMASK(11, 8)
#define ID_MMFR1_EL1_L1HvdSW_SHIFT                      8
#define ID_MMFR1_EL1_L1HvdSW_WIDTH                      4
#define ID_MMFR1_EL1_L1HvdSW_NI                         UL(0b0000)
#define ID_MMFR1_EL1_L1HvdSW_CLEAN_AND_INVALIDATE       UL(0b0001)
#define ID_MMFR1_EL1_L1HvdSW_INVALIDATE_DSIDE_ONLY      UL(0b0010)
#define ID_MMFR1_EL1_L1HvdSW_INVALIDATE                 UL(0b0011)

#define ID_MMFR1_EL1_L1UniVA                            GENMASK(7, 4)
#define ID_MMFR1_EL1_L1UniVA_MASK                       GENMASK(7, 4)
#define ID_MMFR1_EL1_L1UniVA_SHIFT                      4
#define ID_MMFR1_EL1_L1UniVA_WIDTH                      4
#define ID_MMFR1_EL1_L1UniVA_NI                         UL(0b0000)
#define ID_MMFR1_EL1_L1UniVA_CLEAN_AND_INVALIDATE       UL(0b0001)
#define ID_MMFR1_EL1_L1UniVA_INVALIDATE_BP              UL(0b0010)

#define ID_MMFR1_EL1_L1HvdVA                            GENMASK(3, 0)
#define ID_MMFR1_EL1_L1HvdVA_MASK                       GENMASK(3, 0)
#define ID_MMFR1_EL1_L1HvdVA_SHIFT                      0
#define ID_MMFR1_EL1_L1HvdVA_WIDTH                      4
#define ID_MMFR1_EL1_L1HvdVA_NI                         UL(0b0000)
#define ID_MMFR1_EL1_L1HvdVA_CLEAN_AND_INVALIDATE       UL(0b0001)
#define ID_MMFR1_EL1_L1HvdVA_INVALIDATE_BP              UL(0b0010)

#define ID_MMFR1_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_MMFR1_EL1_RES1                               (UL(0))
#define ID_MMFR1_EL1_UNKN                               (UL(0))

#define REG_ID_MMFR2_EL1                                S3_0_C0_C1_6
#define SYS_ID_MMFR2_EL1                                sys_reg(3, 0, 0, 1, 6)
#define SYS_ID_MMFR2_EL1_Op0                            3
#define SYS_ID_MMFR2_EL1_Op1                            0
#define SYS_ID_MMFR2_EL1_CRn                            0
#define SYS_ID_MMFR2_EL1_CRm                            1
#define SYS_ID_MMFR2_EL1_Op2                            6

#define ID_MMFR2_EL1_HWAccFlg                           GENMASK(31, 28)
#define ID_MMFR2_EL1_HWAccFlg_MASK                      GENMASK(31, 28)
#define ID_MMFR2_EL1_HWAccFlg_SHIFT                     28
#define ID_MMFR2_EL1_HWAccFlg_WIDTH                     4
#define ID_MMFR2_EL1_HWAccFlg_NI                        UL(0b0000)
#define ID_MMFR2_EL1_HWAccFlg_IMP                       UL(0b0001)

#define ID_MMFR2_EL1_WFIStall                           GENMASK(27, 24)
#define ID_MMFR2_EL1_WFIStall_MASK                      GENMASK(27, 24)
#define ID_MMFR2_EL1_WFIStall_SHIFT                     24
#define ID_MMFR2_EL1_WFIStall_WIDTH                     4
#define ID_MMFR2_EL1_WFIStall_NI                        UL(0b0000)
#define ID_MMFR2_EL1_WFIStall_IMP                       UL(0b0001)

#define ID_MMFR2_EL1_MemBarr                            GENMASK(23, 20)
#define ID_MMFR2_EL1_MemBarr_MASK                       GENMASK(23, 20)
#define ID_MMFR2_EL1_MemBarr_SHIFT                      20
#define ID_MMFR2_EL1_MemBarr_WIDTH                      4
#define ID_MMFR2_EL1_MemBarr_NI                         UL(0b0000)
#define ID_MMFR2_EL1_MemBarr_DSB_ONLY                   UL(0b0001)
#define ID_MMFR2_EL1_MemBarr_IMP                        UL(0b0010)

#define ID_MMFR2_EL1_UniTLB                             GENMASK(19, 16)
#define ID_MMFR2_EL1_UniTLB_MASK                        GENMASK(19, 16)
#define ID_MMFR2_EL1_UniTLB_SHIFT                       16
#define ID_MMFR2_EL1_UniTLB_WIDTH                       4
#define ID_MMFR2_EL1_UniTLB_NI                          UL(0b0000)
#define ID_MMFR2_EL1_UniTLB_BY_VA                       UL(0b0001)
#define ID_MMFR2_EL1_UniTLB_BY_MATCH_ASID               UL(0b0010)
#define ID_MMFR2_EL1_UniTLB_BY_ALL_ASID                 UL(0b0011)
#define ID_MMFR2_EL1_UniTLB_OTHER_TLBS                  UL(0b0100)
#define ID_MMFR2_EL1_UniTLB_BROADCAST                   UL(0b0101)
#define ID_MMFR2_EL1_UniTLB_BY_IPA                      UL(0b0110)

#define ID_MMFR2_EL1_HvdTLB                             GENMASK(15, 12)
#define ID_MMFR2_EL1_HvdTLB_MASK                        GENMASK(15, 12)
#define ID_MMFR2_EL1_HvdTLB_SHIFT                       12
#define ID_MMFR2_EL1_HvdTLB_WIDTH                       4
#define ID_MMFR2_EL1_HvdTLB_NI                          UL(0b0000)

#define ID_MMFR2_EL1_L1HvdRng                           GENMASK(11, 8)
#define ID_MMFR2_EL1_L1HvdRng_MASK                      GENMASK(11, 8)
#define ID_MMFR2_EL1_L1HvdRng_SHIFT                     8
#define ID_MMFR2_EL1_L1HvdRng_WIDTH                     4
#define ID_MMFR2_EL1_L1HvdRng_NI                        UL(0b0000)
#define ID_MMFR2_EL1_L1HvdRng_IMP                       UL(0b0001)

#define ID_MMFR2_EL1_L1HvdBG                            GENMASK(7, 4)
#define ID_MMFR2_EL1_L1HvdBG_MASK                       GENMASK(7, 4)
#define ID_MMFR2_EL1_L1HvdBG_SHIFT                      4
#define ID_MMFR2_EL1_L1HvdBG_WIDTH                      4
#define ID_MMFR2_EL1_L1HvdBG_NI                         UL(0b0000)
#define ID_MMFR2_EL1_L1HvdBG_IMP                        UL(0b0001)

#define ID_MMFR2_EL1_L1HvdFG                            GENMASK(3, 0)
#define ID_MMFR2_EL1_L1HvdFG_MASK                       GENMASK(3, 0)
#define ID_MMFR2_EL1_L1HvdFG_SHIFT                      0
#define ID_MMFR2_EL1_L1HvdFG_WIDTH                      4
#define ID_MMFR2_EL1_L1HvdFG_NI                         UL(0b0000)
#define ID_MMFR2_EL1_L1HvdFG_IMP                        UL(0b0001)

#define ID_MMFR2_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_MMFR2_EL1_RES1                               (UL(0))
#define ID_MMFR2_EL1_UNKN                               (UL(0))

#define REG_ID_MMFR3_EL1                                S3_0_C0_C1_7
#define SYS_ID_MMFR3_EL1                                sys_reg(3, 0, 0, 1, 7)
#define SYS_ID_MMFR3_EL1_Op0                            3
#define SYS_ID_MMFR3_EL1_Op1                            0
#define SYS_ID_MMFR3_EL1_CRn                            0
#define SYS_ID_MMFR3_EL1_CRm                            1
#define SYS_ID_MMFR3_EL1_Op2                            7

#define ID_MMFR3_EL1_Supersec                           GENMASK(31, 28)
#define ID_MMFR3_EL1_Supersec_MASK                      GENMASK(31, 28)
#define ID_MMFR3_EL1_Supersec_SHIFT                     28
#define ID_MMFR3_EL1_Supersec_WIDTH                     4
#define ID_MMFR3_EL1_Supersec_IMP                       UL(0b0000)
#define ID_MMFR3_EL1_Supersec_NI                        UL(0b1111)

#define ID_MMFR3_EL1_CMemSz                             GENMASK(27, 24)
#define ID_MMFR3_EL1_CMemSz_MASK                        GENMASK(27, 24)
#define ID_MMFR3_EL1_CMemSz_SHIFT                       24
#define ID_MMFR3_EL1_CMemSz_WIDTH                       4
#define ID_MMFR3_EL1_CMemSz_4GB                         UL(0b0000)
#define ID_MMFR3_EL1_CMemSz_64GB                        UL(0b0001)
#define ID_MMFR3_EL1_CMemSz_1TB                         UL(0b0010)

#define ID_MMFR3_EL1_CohWalk                            GENMASK(23, 20)
#define ID_MMFR3_EL1_CohWalk_MASK                       GENMASK(23, 20)
#define ID_MMFR3_EL1_CohWalk_SHIFT                      20
#define ID_MMFR3_EL1_CohWalk_WIDTH                      4
#define ID_MMFR3_EL1_CohWalk_NI                         UL(0b0000)
#define ID_MMFR3_EL1_CohWalk_IMP                        UL(0b0001)

#define ID_MMFR3_EL1_PAN                                GENMASK(19, 16)
#define ID_MMFR3_EL1_PAN_MASK                           GENMASK(19, 16)
#define ID_MMFR3_EL1_PAN_SHIFT                          16
#define ID_MMFR3_EL1_PAN_WIDTH                          4
#define ID_MMFR3_EL1_PAN_NI                             UL(0b0000)
#define ID_MMFR3_EL1_PAN_PAN                            UL(0b0001)
#define ID_MMFR3_EL1_PAN_PAN2                           UL(0b0010)

#define ID_MMFR3_EL1_MaintBcst                          GENMASK(15, 12)
#define ID_MMFR3_EL1_MaintBcst_MASK                     GENMASK(15, 12)
#define ID_MMFR3_EL1_MaintBcst_SHIFT                    12
#define ID_MMFR3_EL1_MaintBcst_WIDTH                    4
#define ID_MMFR3_EL1_MaintBcst_NI                       UL(0b0000)
#define ID_MMFR3_EL1_MaintBcst_NO_TLB                   UL(0b0001)
#define ID_MMFR3_EL1_MaintBcst_ALL                      UL(0b0010)

#define ID_MMFR3_EL1_BPMaint                            GENMASK(11, 8)
#define ID_MMFR3_EL1_BPMaint_MASK                       GENMASK(11, 8)
#define ID_MMFR3_EL1_BPMaint_SHIFT                      8
#define ID_MMFR3_EL1_BPMaint_WIDTH                      4
#define ID_MMFR3_EL1_BPMaint_NI                         UL(0b0000)
#define ID_MMFR3_EL1_BPMaint_ALL                        UL(0b0001)
#define ID_MMFR3_EL1_BPMaint_BY_VA                      UL(0b0010)

#define ID_MMFR3_EL1_CMaintSW                           GENMASK(7, 4)
#define ID_MMFR3_EL1_CMaintSW_MASK                      GENMASK(7, 4)
#define ID_MMFR3_EL1_CMaintSW_SHIFT                     4
#define ID_MMFR3_EL1_CMaintSW_WIDTH                     4
#define ID_MMFR3_EL1_CMaintSW_NI                        UL(0b0000)
#define ID_MMFR3_EL1_CMaintSW_IMP                       UL(0b0001)

#define ID_MMFR3_EL1_CMaintVA                           GENMASK(3, 0)
#define ID_MMFR3_EL1_CMaintVA_MASK                      GENMASK(3, 0)
#define ID_MMFR3_EL1_CMaintVA_SHIFT                     0
#define ID_MMFR3_EL1_CMaintVA_WIDTH                     4
#define ID_MMFR3_EL1_CMaintVA_NI                        UL(0b0000)
#define ID_MMFR3_EL1_CMaintVA_IMP                       UL(0b0001)

#define ID_MMFR3_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_MMFR3_EL1_RES1                               (UL(0))
#define ID_MMFR3_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR0_EL1                                S3_0_C0_C2_0
#define SYS_ID_ISAR0_EL1                                sys_reg(3, 0, 0, 2, 0)
#define SYS_ID_ISAR0_EL1_Op0                            3
#define SYS_ID_ISAR0_EL1_Op1                            0
#define SYS_ID_ISAR0_EL1_CRn                            0
#define SYS_ID_ISAR0_EL1_CRm                            2
#define SYS_ID_ISAR0_EL1_Op2                            0

#define ID_ISAR0_EL1_Divide                             GENMASK(27, 24)
#define ID_ISAR0_EL1_Divide_MASK                        GENMASK(27, 24)
#define ID_ISAR0_EL1_Divide_SHIFT                       24
#define ID_ISAR0_EL1_Divide_WIDTH                       4
#define ID_ISAR0_EL1_Divide_NI                          UL(0b0000)
#define ID_ISAR0_EL1_Divide_xDIV_T32                    UL(0b0001)
#define ID_ISAR0_EL1_Divide_xDIV_A32                    UL(0b0010)

#define ID_ISAR0_EL1_Debug                              GENMASK(23, 20)
#define ID_ISAR0_EL1_Debug_MASK                         GENMASK(23, 20)
#define ID_ISAR0_EL1_Debug_SHIFT                        20
#define ID_ISAR0_EL1_Debug_WIDTH                        4
#define ID_ISAR0_EL1_Debug_SIGNED                       false
#define ID_ISAR0_EL1_Debug_NI                           UL(0b0000)
#define ID_ISAR0_EL1_Debug_IMP                          UL(0b0001)

#define ID_ISAR0_EL1_Coproc                             GENMASK(19, 16)
#define ID_ISAR0_EL1_Coproc_MASK                        GENMASK(19, 16)
#define ID_ISAR0_EL1_Coproc_SHIFT                       16
#define ID_ISAR0_EL1_Coproc_WIDTH                       4
#define ID_ISAR0_EL1_Coproc_NI                          UL(0b0000)
#define ID_ISAR0_EL1_Coproc_MRC                         UL(0b0001)
#define ID_ISAR0_EL1_Coproc_MRC2                        UL(0b0010)
#define ID_ISAR0_EL1_Coproc_MRRC                        UL(0b0011)
#define ID_ISAR0_EL1_Coproc_MRRC2                       UL(0b0100)

#define ID_ISAR0_EL1_CmpBranch                          GENMASK(15, 12)
#define ID_ISAR0_EL1_CmpBranch_MASK                     GENMASK(15, 12)
#define ID_ISAR0_EL1_CmpBranch_SHIFT                    12
#define ID_ISAR0_EL1_CmpBranch_WIDTH                    4
#define ID_ISAR0_EL1_CmpBranch_SIGNED                   false
#define ID_ISAR0_EL1_CmpBranch_NI                       UL(0b0000)
#define ID_ISAR0_EL1_CmpBranch_IMP                      UL(0b0001)

#define ID_ISAR0_EL1_BitField                           GENMASK(11, 8)
#define ID_ISAR0_EL1_BitField_MASK                      GENMASK(11, 8)
#define ID_ISAR0_EL1_BitField_SHIFT                     8
#define ID_ISAR0_EL1_BitField_WIDTH                     4
#define ID_ISAR0_EL1_BitField_SIGNED                    false
#define ID_ISAR0_EL1_BitField_NI                        UL(0b0000)
#define ID_ISAR0_EL1_BitField_IMP                       UL(0b0001)

#define ID_ISAR0_EL1_BitCount                           GENMASK(7, 4)
#define ID_ISAR0_EL1_BitCount_MASK                      GENMASK(7, 4)
#define ID_ISAR0_EL1_BitCount_SHIFT                     4
#define ID_ISAR0_EL1_BitCount_WIDTH                     4
#define ID_ISAR0_EL1_BitCount_SIGNED                    false
#define ID_ISAR0_EL1_BitCount_NI                        UL(0b0000)
#define ID_ISAR0_EL1_BitCount_IMP                       UL(0b0001)

#define ID_ISAR0_EL1_Swap                               GENMASK(3, 0)
#define ID_ISAR0_EL1_Swap_MASK                          GENMASK(3, 0)
#define ID_ISAR0_EL1_Swap_SHIFT                         0
#define ID_ISAR0_EL1_Swap_WIDTH                         4
#define ID_ISAR0_EL1_Swap_SIGNED                        false
#define ID_ISAR0_EL1_Swap_NI                            UL(0b0000)
#define ID_ISAR0_EL1_Swap_IMP                           UL(0b0001)

#define ID_ISAR0_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 28))
#define ID_ISAR0_EL1_RES1                               (UL(0))
#define ID_ISAR0_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR1_EL1                                S3_0_C0_C2_1
#define SYS_ID_ISAR1_EL1                                sys_reg(3, 0, 0, 2, 1)
#define SYS_ID_ISAR1_EL1_Op0                            3
#define SYS_ID_ISAR1_EL1_Op1                            0
#define SYS_ID_ISAR1_EL1_CRn                            0
#define SYS_ID_ISAR1_EL1_CRm                            2
#define SYS_ID_ISAR1_EL1_Op2                            1

#define ID_ISAR1_EL1_Jazelle                            GENMASK(31, 28)
#define ID_ISAR1_EL1_Jazelle_MASK                       GENMASK(31, 28)
#define ID_ISAR1_EL1_Jazelle_SHIFT                      28
#define ID_ISAR1_EL1_Jazelle_WIDTH                      4
#define ID_ISAR1_EL1_Jazelle_NI                         UL(0b0000)
#define ID_ISAR1_EL1_Jazelle_IMP                        UL(0b0001)

#define ID_ISAR1_EL1_Interwork                          GENMASK(27, 24)
#define ID_ISAR1_EL1_Interwork_MASK                     GENMASK(27, 24)
#define ID_ISAR1_EL1_Interwork_SHIFT                    24
#define ID_ISAR1_EL1_Interwork_WIDTH                    4
#define ID_ISAR1_EL1_Interwork_NI                       UL(0b0000)
#define ID_ISAR1_EL1_Interwork_BX                       UL(0b0001)
#define ID_ISAR1_EL1_Interwork_BLX                      UL(0b0010)
#define ID_ISAR1_EL1_Interwork_A32_BX                   UL(0b0011)

#define ID_ISAR1_EL1_Immediate                          GENMASK(23, 20)
#define ID_ISAR1_EL1_Immediate_MASK                     GENMASK(23, 20)
#define ID_ISAR1_EL1_Immediate_SHIFT                    20
#define ID_ISAR1_EL1_Immediate_WIDTH                    4
#define ID_ISAR1_EL1_Immediate_NI                       UL(0b0000)
#define ID_ISAR1_EL1_Immediate_IMP                      UL(0b0001)

#define ID_ISAR1_EL1_IfThen                             GENMASK(19, 16)
#define ID_ISAR1_EL1_IfThen_MASK                        GENMASK(19, 16)
#define ID_ISAR1_EL1_IfThen_SHIFT                       16
#define ID_ISAR1_EL1_IfThen_WIDTH                       4
#define ID_ISAR1_EL1_IfThen_NI                          UL(0b0000)
#define ID_ISAR1_EL1_IfThen_IMP                         UL(0b0001)

#define ID_ISAR1_EL1_Extend                             GENMASK(15, 12)
#define ID_ISAR1_EL1_Extend_MASK                        GENMASK(15, 12)
#define ID_ISAR1_EL1_Extend_SHIFT                       12
#define ID_ISAR1_EL1_Extend_WIDTH                       4
#define ID_ISAR1_EL1_Extend_NI                          UL(0b0000)
#define ID_ISAR1_EL1_Extend_SXTB                        UL(0b0001)
#define ID_ISAR1_EL1_Extend_SXTB16                      UL(0b0010)

#define ID_ISAR1_EL1_Except_AR                          GENMASK(11, 8)
#define ID_ISAR1_EL1_Except_AR_MASK                     GENMASK(11, 8)
#define ID_ISAR1_EL1_Except_AR_SHIFT                    8
#define ID_ISAR1_EL1_Except_AR_WIDTH                    4
#define ID_ISAR1_EL1_Except_AR_NI                       UL(0b0000)
#define ID_ISAR1_EL1_Except_AR_IMP                      UL(0b0001)

#define ID_ISAR1_EL1_Except                             GENMASK(7, 4)
#define ID_ISAR1_EL1_Except_MASK                        GENMASK(7, 4)
#define ID_ISAR1_EL1_Except_SHIFT                       4
#define ID_ISAR1_EL1_Except_WIDTH                       4
#define ID_ISAR1_EL1_Except_NI                          UL(0b0000)
#define ID_ISAR1_EL1_Except_IMP                         UL(0b0001)

#define ID_ISAR1_EL1_Endian                             GENMASK(3, 0)
#define ID_ISAR1_EL1_Endian_MASK                        GENMASK(3, 0)
#define ID_ISAR1_EL1_Endian_SHIFT                       0
#define ID_ISAR1_EL1_Endian_WIDTH                       4
#define ID_ISAR1_EL1_Endian_NI                          UL(0b0000)
#define ID_ISAR1_EL1_Endian_IMP                         UL(0b0001)

#define ID_ISAR1_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_ISAR1_EL1_RES1                               (UL(0))
#define ID_ISAR1_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR2_EL1                                S3_0_C0_C2_2
#define SYS_ID_ISAR2_EL1                                sys_reg(3, 0, 0, 2, 2)
#define SYS_ID_ISAR2_EL1_Op0                            3
#define SYS_ID_ISAR2_EL1_Op1                            0
#define SYS_ID_ISAR2_EL1_CRn                            0
#define SYS_ID_ISAR2_EL1_CRm                            2
#define SYS_ID_ISAR2_EL1_Op2                            2

#define ID_ISAR2_EL1_Reversal                           GENMASK(31, 28)
#define ID_ISAR2_EL1_Reversal_MASK                      GENMASK(31, 28)
#define ID_ISAR2_EL1_Reversal_SHIFT                     28
#define ID_ISAR2_EL1_Reversal_WIDTH                     4
#define ID_ISAR2_EL1_Reversal_NI                        UL(0b0000)
#define ID_ISAR2_EL1_Reversal_REV                       UL(0b0001)
#define ID_ISAR2_EL1_Reversal_RBIT                      UL(0b0010)

#define ID_ISAR2_EL1_PSR_AR                             GENMASK(27, 24)
#define ID_ISAR2_EL1_PSR_AR_MASK                        GENMASK(27, 24)
#define ID_ISAR2_EL1_PSR_AR_SHIFT                       24
#define ID_ISAR2_EL1_PSR_AR_WIDTH                       4
#define ID_ISAR2_EL1_PSR_AR_NI                          UL(0b0000)
#define ID_ISAR2_EL1_PSR_AR_IMP                         UL(0b0001)

#define ID_ISAR2_EL1_MultU                              GENMASK(23, 20)
#define ID_ISAR2_EL1_MultU_MASK                         GENMASK(23, 20)
#define ID_ISAR2_EL1_MultU_SHIFT                        20
#define ID_ISAR2_EL1_MultU_WIDTH                        4
#define ID_ISAR2_EL1_MultU_NI                           UL(0b0000)
#define ID_ISAR2_EL1_MultU_UMULL                        UL(0b0001)
#define ID_ISAR2_EL1_MultU_UMAAL                        UL(0b0010)

#define ID_ISAR2_EL1_MultS                              GENMASK(19, 16)
#define ID_ISAR2_EL1_MultS_MASK                         GENMASK(19, 16)
#define ID_ISAR2_EL1_MultS_SHIFT                        16
#define ID_ISAR2_EL1_MultS_WIDTH                        4
#define ID_ISAR2_EL1_MultS_NI                           UL(0b0000)
#define ID_ISAR2_EL1_MultS_SMULL                        UL(0b0001)
#define ID_ISAR2_EL1_MultS_SMLABB                       UL(0b0010)
#define ID_ISAR2_EL1_MultS_SMLAD                        UL(0b0011)

#define ID_ISAR2_EL1_Mult                               GENMASK(15, 12)
#define ID_ISAR2_EL1_Mult_MASK                          GENMASK(15, 12)
#define ID_ISAR2_EL1_Mult_SHIFT                         12
#define ID_ISAR2_EL1_Mult_WIDTH                         4
#define ID_ISAR2_EL1_Mult_NI                            UL(0b0000)
#define ID_ISAR2_EL1_Mult_MLA                           UL(0b0001)
#define ID_ISAR2_EL1_Mult_MLS                           UL(0b0010)

#define ID_ISAR2_EL1_MultiAccessInt                     GENMASK(11, 8)
#define ID_ISAR2_EL1_MultiAccessInt_MASK                GENMASK(11, 8)
#define ID_ISAR2_EL1_MultiAccessInt_SHIFT               8
#define ID_ISAR2_EL1_MultiAccessInt_WIDTH               4
#define ID_ISAR2_EL1_MultiAccessInt_NI                  UL(0b0000)
#define ID_ISAR2_EL1_MultiAccessInt_RESTARTABLE         UL(0b0001)
#define ID_ISAR2_EL1_MultiAccessInt_CONTINUABLE         UL(0b0010)

#define ID_ISAR2_EL1_MemHint                            GENMASK(7, 4)
#define ID_ISAR2_EL1_MemHint_MASK                       GENMASK(7, 4)
#define ID_ISAR2_EL1_MemHint_SHIFT                      4
#define ID_ISAR2_EL1_MemHint_WIDTH                      4
#define ID_ISAR2_EL1_MemHint_NI                         UL(0b0000)
#define ID_ISAR2_EL1_MemHint_PLD                        UL(0b0001)
#define ID_ISAR2_EL1_MemHint_PLD2                       UL(0b0010)
#define ID_ISAR2_EL1_MemHint_PLI                        UL(0b0011)
#define ID_ISAR2_EL1_MemHint_PLDW                       UL(0b0100)

#define ID_ISAR2_EL1_LoadStore                          GENMASK(3, 0)
#define ID_ISAR2_EL1_LoadStore_MASK                     GENMASK(3, 0)
#define ID_ISAR2_EL1_LoadStore_SHIFT                    0
#define ID_ISAR2_EL1_LoadStore_WIDTH                    4
#define ID_ISAR2_EL1_LoadStore_NI                       UL(0b0000)
#define ID_ISAR2_EL1_LoadStore_DOUBLE                   UL(0b0001)
#define ID_ISAR2_EL1_LoadStore_ACQUIRE                  UL(0b0010)

#define ID_ISAR2_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_ISAR2_EL1_RES1                               (UL(0))
#define ID_ISAR2_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR3_EL1                                S3_0_C0_C2_3
#define SYS_ID_ISAR3_EL1                                sys_reg(3, 0, 0, 2, 3)
#define SYS_ID_ISAR3_EL1_Op0                            3
#define SYS_ID_ISAR3_EL1_Op1                            0
#define SYS_ID_ISAR3_EL1_CRn                            0
#define SYS_ID_ISAR3_EL1_CRm                            2
#define SYS_ID_ISAR3_EL1_Op2                            3

#define ID_ISAR3_EL1_T32EE                              GENMASK(31, 28)
#define ID_ISAR3_EL1_T32EE_MASK                         GENMASK(31, 28)
#define ID_ISAR3_EL1_T32EE_SHIFT                        28
#define ID_ISAR3_EL1_T32EE_WIDTH                        4
#define ID_ISAR3_EL1_T32EE_NI                           UL(0b0000)
#define ID_ISAR3_EL1_T32EE_IMP                          UL(0b0001)

#define ID_ISAR3_EL1_TrueNOP                            GENMASK(27, 24)
#define ID_ISAR3_EL1_TrueNOP_MASK                       GENMASK(27, 24)
#define ID_ISAR3_EL1_TrueNOP_SHIFT                      24
#define ID_ISAR3_EL1_TrueNOP_WIDTH                      4
#define ID_ISAR3_EL1_TrueNOP_NI                         UL(0b0000)
#define ID_ISAR3_EL1_TrueNOP_IMP                        UL(0b0001)

#define ID_ISAR3_EL1_T32Copy                            GENMASK(23, 20)
#define ID_ISAR3_EL1_T32Copy_MASK                       GENMASK(23, 20)
#define ID_ISAR3_EL1_T32Copy_SHIFT                      20
#define ID_ISAR3_EL1_T32Copy_WIDTH                      4
#define ID_ISAR3_EL1_T32Copy_NI                         UL(0b0000)
#define ID_ISAR3_EL1_T32Copy_IMP                        UL(0b0001)

#define ID_ISAR3_EL1_TabBranch                          GENMASK(19, 16)
#define ID_ISAR3_EL1_TabBranch_MASK                     GENMASK(19, 16)
#define ID_ISAR3_EL1_TabBranch_SHIFT                    16
#define ID_ISAR3_EL1_TabBranch_WIDTH                    4
#define ID_ISAR3_EL1_TabBranch_NI                       UL(0b0000)
#define ID_ISAR3_EL1_TabBranch_IMP                      UL(0b0001)

#define ID_ISAR3_EL1_SynchPrim                          GENMASK(15, 12)
#define ID_ISAR3_EL1_SynchPrim_MASK                     GENMASK(15, 12)
#define ID_ISAR3_EL1_SynchPrim_SHIFT                    12
#define ID_ISAR3_EL1_SynchPrim_WIDTH                    4
#define ID_ISAR3_EL1_SynchPrim_NI                       UL(0b0000)
#define ID_ISAR3_EL1_SynchPrim_EXCLUSIVE                UL(0b0001)
#define ID_ISAR3_EL1_SynchPrim_DOUBLE                   UL(0b0010)

#define ID_ISAR3_EL1_SVC                                GENMASK(11, 8)
#define ID_ISAR3_EL1_SVC_MASK                           GENMASK(11, 8)
#define ID_ISAR3_EL1_SVC_SHIFT                          8
#define ID_ISAR3_EL1_SVC_WIDTH                          4
#define ID_ISAR3_EL1_SVC_NI                             UL(0b0000)
#define ID_ISAR3_EL1_SVC_IMP                            UL(0b0001)

#define ID_ISAR3_EL1_SIMD                               GENMASK(7, 4)
#define ID_ISAR3_EL1_SIMD_MASK                          GENMASK(7, 4)
#define ID_ISAR3_EL1_SIMD_SHIFT                         4
#define ID_ISAR3_EL1_SIMD_WIDTH                         4
#define ID_ISAR3_EL1_SIMD_NI                            UL(0b0000)
#define ID_ISAR3_EL1_SIMD_SSAT                          UL(0b0001)
#define ID_ISAR3_EL1_SIMD_PKHBT                         UL(0b0011)

#define ID_ISAR3_EL1_Saturate                           GENMASK(3, 0)
#define ID_ISAR3_EL1_Saturate_MASK                      GENMASK(3, 0)
#define ID_ISAR3_EL1_Saturate_SHIFT                     0
#define ID_ISAR3_EL1_Saturate_WIDTH                     4
#define ID_ISAR3_EL1_Saturate_NI                        UL(0b0000)
#define ID_ISAR3_EL1_Saturate_IMP                       UL(0b0001)

#define ID_ISAR3_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_ISAR3_EL1_RES1                               (UL(0))
#define ID_ISAR3_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR4_EL1                                S3_0_C0_C2_4
#define SYS_ID_ISAR4_EL1                                sys_reg(3, 0, 0, 2, 4)
#define SYS_ID_ISAR4_EL1_Op0                            3
#define SYS_ID_ISAR4_EL1_Op1                            0
#define SYS_ID_ISAR4_EL1_CRn                            0
#define SYS_ID_ISAR4_EL1_CRm                            2
#define SYS_ID_ISAR4_EL1_Op2                            4

#define ID_ISAR4_EL1_SWP_frac                           GENMASK(31, 28)
#define ID_ISAR4_EL1_SWP_frac_MASK                      GENMASK(31, 28)
#define ID_ISAR4_EL1_SWP_frac_SHIFT                     28
#define ID_ISAR4_EL1_SWP_frac_WIDTH                     4
#define ID_ISAR4_EL1_SWP_frac_NI                        UL(0b0000)
#define ID_ISAR4_EL1_SWP_frac_IMP                       UL(0b0001)

#define ID_ISAR4_EL1_PSR_M                              GENMASK(27, 24)
#define ID_ISAR4_EL1_PSR_M_MASK                         GENMASK(27, 24)
#define ID_ISAR4_EL1_PSR_M_SHIFT                        24
#define ID_ISAR4_EL1_PSR_M_WIDTH                        4
#define ID_ISAR4_EL1_PSR_M_NI                           UL(0b0000)
#define ID_ISAR4_EL1_PSR_M_IMP                          UL(0b0001)

#define ID_ISAR4_EL1_SynchPrim_frac                     GENMASK(23, 20)
#define ID_ISAR4_EL1_SynchPrim_frac_MASK                GENMASK(23, 20)
#define ID_ISAR4_EL1_SynchPrim_frac_SHIFT               20
#define ID_ISAR4_EL1_SynchPrim_frac_WIDTH               4
#define ID_ISAR4_EL1_SynchPrim_frac_NI                  UL(0b0000)
#define ID_ISAR4_EL1_SynchPrim_frac_IMP                 UL(0b0011)

#define ID_ISAR4_EL1_Barrier                            GENMASK(19, 16)
#define ID_ISAR4_EL1_Barrier_MASK                       GENMASK(19, 16)
#define ID_ISAR4_EL1_Barrier_SHIFT                      16
#define ID_ISAR4_EL1_Barrier_WIDTH                      4
#define ID_ISAR4_EL1_Barrier_NI                         UL(0b0000)
#define ID_ISAR4_EL1_Barrier_IMP                        UL(0b0001)

#define ID_ISAR4_EL1_SMC                                GENMASK(15, 12)
#define ID_ISAR4_EL1_SMC_MASK                           GENMASK(15, 12)
#define ID_ISAR4_EL1_SMC_SHIFT                          12
#define ID_ISAR4_EL1_SMC_WIDTH                          4
#define ID_ISAR4_EL1_SMC_NI                             UL(0b0000)
#define ID_ISAR4_EL1_SMC_IMP                            UL(0b0001)

#define ID_ISAR4_EL1_Writeback                          GENMASK(11, 8)
#define ID_ISAR4_EL1_Writeback_MASK                     GENMASK(11, 8)
#define ID_ISAR4_EL1_Writeback_SHIFT                    8
#define ID_ISAR4_EL1_Writeback_WIDTH                    4
#define ID_ISAR4_EL1_Writeback_NI                       UL(0b0000)
#define ID_ISAR4_EL1_Writeback_IMP                      UL(0b0001)

#define ID_ISAR4_EL1_WithShifts                         GENMASK(7, 4)
#define ID_ISAR4_EL1_WithShifts_MASK                    GENMASK(7, 4)
#define ID_ISAR4_EL1_WithShifts_SHIFT                   4
#define ID_ISAR4_EL1_WithShifts_WIDTH                   4
#define ID_ISAR4_EL1_WithShifts_NI                      UL(0b0000)
#define ID_ISAR4_EL1_WithShifts_LSL3                    UL(0b0001)
#define ID_ISAR4_EL1_WithShifts_LS                      UL(0b0011)
#define ID_ISAR4_EL1_WithShifts_REG                     UL(0b0100)

#define ID_ISAR4_EL1_Unpriv                             GENMASK(3, 0)
#define ID_ISAR4_EL1_Unpriv_MASK                        GENMASK(3, 0)
#define ID_ISAR4_EL1_Unpriv_SHIFT                       0
#define ID_ISAR4_EL1_Unpriv_WIDTH                       4
#define ID_ISAR4_EL1_Unpriv_NI                          UL(0b0000)
#define ID_ISAR4_EL1_Unpriv_REG_BYTE                    UL(0b0001)
#define ID_ISAR4_EL1_Unpriv_SIGNED_HALFWORD             UL(0b0010)

#define ID_ISAR4_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_ISAR4_EL1_RES1                               (UL(0))
#define ID_ISAR4_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR5_EL1                                S3_0_C0_C2_5
#define SYS_ID_ISAR5_EL1                                sys_reg(3, 0, 0, 2, 5)
#define SYS_ID_ISAR5_EL1_Op0                            3
#define SYS_ID_ISAR5_EL1_Op1                            0
#define SYS_ID_ISAR5_EL1_CRn                            0
#define SYS_ID_ISAR5_EL1_CRm                            2
#define SYS_ID_ISAR5_EL1_Op2                            5

#define ID_ISAR5_EL1_VCMA                               GENMASK(31, 28)
#define ID_ISAR5_EL1_VCMA_MASK                          GENMASK(31, 28)
#define ID_ISAR5_EL1_VCMA_SHIFT                         28
#define ID_ISAR5_EL1_VCMA_WIDTH                         4
#define ID_ISAR5_EL1_VCMA_SIGNED                        false
#define ID_ISAR5_EL1_VCMA_NI                            UL(0b0000)
#define ID_ISAR5_EL1_VCMA_IMP                           UL(0b0001)

#define ID_ISAR5_EL1_RDM                                GENMASK(27, 24)
#define ID_ISAR5_EL1_RDM_MASK                           GENMASK(27, 24)
#define ID_ISAR5_EL1_RDM_SHIFT                          24
#define ID_ISAR5_EL1_RDM_WIDTH                          4
#define ID_ISAR5_EL1_RDM_SIGNED                         false
#define ID_ISAR5_EL1_RDM_NI                             UL(0b0000)
#define ID_ISAR5_EL1_RDM_IMP                            UL(0b0001)

#define ID_ISAR5_EL1_CRC32                              GENMASK(19, 16)
#define ID_ISAR5_EL1_CRC32_MASK                         GENMASK(19, 16)
#define ID_ISAR5_EL1_CRC32_SHIFT                        16
#define ID_ISAR5_EL1_CRC32_WIDTH                        4
#define ID_ISAR5_EL1_CRC32_SIGNED                       false
#define ID_ISAR5_EL1_CRC32_NI                           UL(0b0000)
#define ID_ISAR5_EL1_CRC32_IMP                          UL(0b0001)

#define ID_ISAR5_EL1_SHA2                               GENMASK(15, 12)
#define ID_ISAR5_EL1_SHA2_MASK                          GENMASK(15, 12)
#define ID_ISAR5_EL1_SHA2_SHIFT                         12
#define ID_ISAR5_EL1_SHA2_WIDTH                         4
#define ID_ISAR5_EL1_SHA2_SIGNED                        false
#define ID_ISAR5_EL1_SHA2_NI                            UL(0b0000)
#define ID_ISAR5_EL1_SHA2_IMP                           UL(0b0001)

#define ID_ISAR5_EL1_SHA1                               GENMASK(11, 8)
#define ID_ISAR5_EL1_SHA1_MASK                          GENMASK(11, 8)
#define ID_ISAR5_EL1_SHA1_SHIFT                         8
#define ID_ISAR5_EL1_SHA1_WIDTH                         4
#define ID_ISAR5_EL1_SHA1_SIGNED                        false
#define ID_ISAR5_EL1_SHA1_NI                            UL(0b0000)
#define ID_ISAR5_EL1_SHA1_IMP                           UL(0b0001)

#define ID_ISAR5_EL1_AES                                GENMASK(7, 4)
#define ID_ISAR5_EL1_AES_MASK                           GENMASK(7, 4)
#define ID_ISAR5_EL1_AES_SHIFT                          4
#define ID_ISAR5_EL1_AES_WIDTH                          4
#define ID_ISAR5_EL1_AES_SIGNED                         false
#define ID_ISAR5_EL1_AES_NI                             UL(0b0000)
#define ID_ISAR5_EL1_AES_IMP                            UL(0b0001)
#define ID_ISAR5_EL1_AES_VMULL                          UL(0b0010)

#define ID_ISAR5_EL1_SEVL                               GENMASK(3, 0)
#define ID_ISAR5_EL1_SEVL_MASK                          GENMASK(3, 0)
#define ID_ISAR5_EL1_SEVL_SHIFT                         0
#define ID_ISAR5_EL1_SEVL_WIDTH                         4
#define ID_ISAR5_EL1_SEVL_SIGNED                        false
#define ID_ISAR5_EL1_SEVL_NI                            UL(0b0000)
#define ID_ISAR5_EL1_SEVL_IMP                           UL(0b0001)

#define ID_ISAR5_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32) | GENMASK_ULL(23, 20))
#define ID_ISAR5_EL1_RES1                               (UL(0))
#define ID_ISAR5_EL1_UNKN                               (UL(0))

#define REG_ID_ISAR6_EL1                                S3_0_C0_C2_7
#define SYS_ID_ISAR6_EL1                                sys_reg(3, 0, 0, 2, 7)
#define SYS_ID_ISAR6_EL1_Op0                            3
#define SYS_ID_ISAR6_EL1_Op1                            0
#define SYS_ID_ISAR6_EL1_CRn                            0
#define SYS_ID_ISAR6_EL1_CRm                            2
#define SYS_ID_ISAR6_EL1_Op2                            7

#define ID_ISAR6_EL1_I8MM                               GENMASK(27, 24)
#define ID_ISAR6_EL1_I8MM_MASK                          GENMASK(27, 24)
#define ID_ISAR6_EL1_I8MM_SHIFT                         24
#define ID_ISAR6_EL1_I8MM_WIDTH                         4
#define ID_ISAR6_EL1_I8MM_SIGNED                        false
#define ID_ISAR6_EL1_I8MM_NI                            UL(0b0000)
#define ID_ISAR6_EL1_I8MM_IMP                           UL(0b0001)

#define ID_ISAR6_EL1_BF16                               GENMASK(23, 20)
#define ID_ISAR6_EL1_BF16_MASK                          GENMASK(23, 20)
#define ID_ISAR6_EL1_BF16_SHIFT                         20
#define ID_ISAR6_EL1_BF16_WIDTH                         4
#define ID_ISAR6_EL1_BF16_SIGNED                        false
#define ID_ISAR6_EL1_BF16_NI                            UL(0b0000)
#define ID_ISAR6_EL1_BF16_IMP                           UL(0b0001)

#define ID_ISAR6_EL1_SPECRES                            GENMASK(19, 16)
#define ID_ISAR6_EL1_SPECRES_MASK                       GENMASK(19, 16)
#define ID_ISAR6_EL1_SPECRES_SHIFT                      16
#define ID_ISAR6_EL1_SPECRES_WIDTH                      4
#define ID_ISAR6_EL1_SPECRES_SIGNED                     false
#define ID_ISAR6_EL1_SPECRES_NI                         UL(0b0000)
#define ID_ISAR6_EL1_SPECRES_IMP                        UL(0b0001)

#define ID_ISAR6_EL1_SB                                 GENMASK(15, 12)
#define ID_ISAR6_EL1_SB_MASK                            GENMASK(15, 12)
#define ID_ISAR6_EL1_SB_SHIFT                           12
#define ID_ISAR6_EL1_SB_WIDTH                           4
#define ID_ISAR6_EL1_SB_SIGNED                          false
#define ID_ISAR6_EL1_SB_NI                              UL(0b0000)
#define ID_ISAR6_EL1_SB_IMP                             UL(0b0001)

#define ID_ISAR6_EL1_FHM                                GENMASK(11, 8)
#define ID_ISAR6_EL1_FHM_MASK                           GENMASK(11, 8)
#define ID_ISAR6_EL1_FHM_SHIFT                          8
#define ID_ISAR6_EL1_FHM_WIDTH                          4
#define ID_ISAR6_EL1_FHM_SIGNED                         false
#define ID_ISAR6_EL1_FHM_NI                             UL(0b0000)
#define ID_ISAR6_EL1_FHM_IMP                            UL(0b0001)

#define ID_ISAR6_EL1_DP                                 GENMASK(7, 4)
#define ID_ISAR6_EL1_DP_MASK                            GENMASK(7, 4)
#define ID_ISAR6_EL1_DP_SHIFT                           4
#define ID_ISAR6_EL1_DP_WIDTH                           4
#define ID_ISAR6_EL1_DP_SIGNED                          false
#define ID_ISAR6_EL1_DP_NI                              UL(0b0000)
#define ID_ISAR6_EL1_DP_IMP                             UL(0b0001)

#define ID_ISAR6_EL1_JSCVT                              GENMASK(3, 0)
#define ID_ISAR6_EL1_JSCVT_MASK                         GENMASK(3, 0)
#define ID_ISAR6_EL1_JSCVT_SHIFT                        0
#define ID_ISAR6_EL1_JSCVT_WIDTH                        4
#define ID_ISAR6_EL1_JSCVT_SIGNED                       false
#define ID_ISAR6_EL1_JSCVT_NI                           UL(0b0000)
#define ID_ISAR6_EL1_JSCVT_IMP                          UL(0b0001)

#define ID_ISAR6_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 28))
#define ID_ISAR6_EL1_RES1                               (UL(0))
#define ID_ISAR6_EL1_UNKN                               (UL(0))

#define REG_ID_MMFR4_EL1                                S3_0_C0_C2_6
#define SYS_ID_MMFR4_EL1                                sys_reg(3, 0, 0, 2, 6)
#define SYS_ID_MMFR4_EL1_Op0                            3
#define SYS_ID_MMFR4_EL1_Op1                            0
#define SYS_ID_MMFR4_EL1_CRn                            0
#define SYS_ID_MMFR4_EL1_CRm                            2
#define SYS_ID_MMFR4_EL1_Op2                            6

#define ID_MMFR4_EL1_EVT                                GENMASK(31, 28)
#define ID_MMFR4_EL1_EVT_MASK                           GENMASK(31, 28)
#define ID_MMFR4_EL1_EVT_SHIFT                          28
#define ID_MMFR4_EL1_EVT_WIDTH                          4
#define ID_MMFR4_EL1_EVT_SIGNED                         false
#define ID_MMFR4_EL1_EVT_NI                             UL(0b0000)
#define ID_MMFR4_EL1_EVT_NO_TLBIS                       UL(0b0001)
#define ID_MMFR4_EL1_EVT_TLBIS                          UL(0b0010)

#define ID_MMFR4_EL1_CCIDX                              GENMASK(27, 24)
#define ID_MMFR4_EL1_CCIDX_MASK                         GENMASK(27, 24)
#define ID_MMFR4_EL1_CCIDX_SHIFT                        24
#define ID_MMFR4_EL1_CCIDX_WIDTH                        4
#define ID_MMFR4_EL1_CCIDX_SIGNED                       false
#define ID_MMFR4_EL1_CCIDX_NI                           UL(0b0000)
#define ID_MMFR4_EL1_CCIDX_IMP                          UL(0b0001)

#define ID_MMFR4_EL1_LSM                                GENMASK(23, 20)
#define ID_MMFR4_EL1_LSM_MASK                           GENMASK(23, 20)
#define ID_MMFR4_EL1_LSM_SHIFT                          20
#define ID_MMFR4_EL1_LSM_WIDTH                          4
#define ID_MMFR4_EL1_LSM_SIGNED                         false
#define ID_MMFR4_EL1_LSM_NI                             UL(0b0000)
#define ID_MMFR4_EL1_LSM_IMP                            UL(0b0001)

#define ID_MMFR4_EL1_HPDS                               GENMASK(19, 16)
#define ID_MMFR4_EL1_HPDS_MASK                          GENMASK(19, 16)
#define ID_MMFR4_EL1_HPDS_SHIFT                         16
#define ID_MMFR4_EL1_HPDS_WIDTH                         4
#define ID_MMFR4_EL1_HPDS_SIGNED                        false
#define ID_MMFR4_EL1_HPDS_NI                            UL(0b0000)
#define ID_MMFR4_EL1_HPDS_AA32HPD                       UL(0b0001)
#define ID_MMFR4_EL1_HPDS_HPDS2                         UL(0b0010)

#define ID_MMFR4_EL1_CnP                                GENMASK(15, 12)
#define ID_MMFR4_EL1_CnP_MASK                           GENMASK(15, 12)
#define ID_MMFR4_EL1_CnP_SHIFT                          12
#define ID_MMFR4_EL1_CnP_WIDTH                          4
#define ID_MMFR4_EL1_CnP_SIGNED                         false
#define ID_MMFR4_EL1_CnP_NI                             UL(0b0000)
#define ID_MMFR4_EL1_CnP_IMP                            UL(0b0001)

#define ID_MMFR4_EL1_XNX                                GENMASK(11, 8)
#define ID_MMFR4_EL1_XNX_MASK                           GENMASK(11, 8)
#define ID_MMFR4_EL1_XNX_SHIFT                          8
#define ID_MMFR4_EL1_XNX_WIDTH                          4
#define ID_MMFR4_EL1_XNX_SIGNED                         false
#define ID_MMFR4_EL1_XNX_NI                             UL(0b0000)
#define ID_MMFR4_EL1_XNX_IMP                            UL(0b0001)

#define ID_MMFR4_EL1_AC2                                GENMASK(7, 4)
#define ID_MMFR4_EL1_AC2_MASK                           GENMASK(7, 4)
#define ID_MMFR4_EL1_AC2_SHIFT                          4
#define ID_MMFR4_EL1_AC2_WIDTH                          4
#define ID_MMFR4_EL1_AC2_SIGNED                         false
#define ID_MMFR4_EL1_AC2_NI                             UL(0b0000)
#define ID_MMFR4_EL1_AC2_IMP                            UL(0b0001)

#define ID_MMFR4_EL1_SpecSEI                            GENMASK(3, 0)
#define ID_MMFR4_EL1_SpecSEI_MASK                       GENMASK(3, 0)
#define ID_MMFR4_EL1_SpecSEI_SHIFT                      0
#define ID_MMFR4_EL1_SpecSEI_WIDTH                      4
#define ID_MMFR4_EL1_SpecSEI_SIGNED                     false
#define ID_MMFR4_EL1_SpecSEI_NI                         UL(0b0000)
#define ID_MMFR4_EL1_SpecSEI_IMP                        UL(0b0001)

#define ID_MMFR4_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 32))
#define ID_MMFR4_EL1_RES1                               (UL(0))
#define ID_MMFR4_EL1_UNKN                               (UL(0))

#define REG_MVFR0_EL1                                   S3_0_C0_C3_0
#define SYS_MVFR0_EL1                                   sys_reg(3, 0, 0, 3, 0)
#define SYS_MVFR0_EL1_Op0                               3
#define SYS_MVFR0_EL1_Op1                               0
#define SYS_MVFR0_EL1_CRn                               0
#define SYS_MVFR0_EL1_CRm                               3
#define SYS_MVFR0_EL1_Op2                               0

#define MVFR0_EL1_FPRound                               GENMASK(31, 28)
#define MVFR0_EL1_FPRound_MASK                          GENMASK(31, 28)
#define MVFR0_EL1_FPRound_SHIFT                         28
#define MVFR0_EL1_FPRound_WIDTH                         4
#define MVFR0_EL1_FPRound_SIGNED                        false
#define MVFR0_EL1_FPRound_NI                            UL(0b0000)
#define MVFR0_EL1_FPRound_IMP                           UL(0b0001)

#define MVFR0_EL1_FPShVec                               GENMASK(27, 24)
#define MVFR0_EL1_FPShVec_MASK                          GENMASK(27, 24)
#define MVFR0_EL1_FPShVec_SHIFT                         24
#define MVFR0_EL1_FPShVec_WIDTH                         4
#define MVFR0_EL1_FPShVec_SIGNED                        false
#define MVFR0_EL1_FPShVec_NI                            UL(0b0000)
#define MVFR0_EL1_FPShVec_IMP                           UL(0b0001)

#define MVFR0_EL1_FPSqrt                                GENMASK(23, 20)
#define MVFR0_EL1_FPSqrt_MASK                           GENMASK(23, 20)
#define MVFR0_EL1_FPSqrt_SHIFT                          20
#define MVFR0_EL1_FPSqrt_WIDTH                          4
#define MVFR0_EL1_FPSqrt_SIGNED                         false
#define MVFR0_EL1_FPSqrt_NI                             UL(0b0000)
#define MVFR0_EL1_FPSqrt_IMP                            UL(0b0001)

#define MVFR0_EL1_FPDivide                              GENMASK(19, 16)
#define MVFR0_EL1_FPDivide_MASK                         GENMASK(19, 16)
#define MVFR0_EL1_FPDivide_SHIFT                        16
#define MVFR0_EL1_FPDivide_WIDTH                        4
#define MVFR0_EL1_FPDivide_SIGNED                       false
#define MVFR0_EL1_FPDivide_NI                           UL(0b0000)
#define MVFR0_EL1_FPDivide_IMP                          UL(0b0001)

#define MVFR0_EL1_FPTrap                                GENMASK(15, 12)
#define MVFR0_EL1_FPTrap_MASK                           GENMASK(15, 12)
#define MVFR0_EL1_FPTrap_SHIFT                          12
#define MVFR0_EL1_FPTrap_WIDTH                          4
#define MVFR0_EL1_FPTrap_SIGNED                         false
#define MVFR0_EL1_FPTrap_NI                             UL(0b0000)
#define MVFR0_EL1_FPTrap_IMP                            UL(0b0001)

#define MVFR0_EL1_FPDP                                  GENMASK(11, 8)
#define MVFR0_EL1_FPDP_MASK                             GENMASK(11, 8)
#define MVFR0_EL1_FPDP_SHIFT                            8
#define MVFR0_EL1_FPDP_WIDTH                            4
#define MVFR0_EL1_FPDP_SIGNED                           false
#define MVFR0_EL1_FPDP_NI                               UL(0b0000)
#define MVFR0_EL1_FPDP_VFPv2                            UL(0b0001)
#define MVFR0_EL1_FPDP_VFPv3                            UL(0b0010)

#define MVFR0_EL1_FPSP                                  GENMASK(7, 4)
#define MVFR0_EL1_FPSP_MASK                             GENMASK(7, 4)
#define MVFR0_EL1_FPSP_SHIFT                            4
#define MVFR0_EL1_FPSP_WIDTH                            4
#define MVFR0_EL1_FPSP_SIGNED                           false
#define MVFR0_EL1_FPSP_NI                               UL(0b0000)
#define MVFR0_EL1_FPSP_VFPv2                            UL(0b0001)
#define MVFR0_EL1_FPSP_VFPv3                            UL(0b0010)

#define MVFR0_EL1_SIMDReg                               GENMASK(3, 0)
#define MVFR0_EL1_SIMDReg_MASK                          GENMASK(3, 0)
#define MVFR0_EL1_SIMDReg_SHIFT                         0
#define MVFR0_EL1_SIMDReg_WIDTH                         4
#define MVFR0_EL1_SIMDReg_NI                            UL(0b0000)
#define MVFR0_EL1_SIMDReg_IMP_16x64                     UL(0b0001)
#define MVFR0_EL1_SIMDReg_IMP_32x64                     UL(0b0010)

#define MVFR0_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 32))
#define MVFR0_EL1_RES1                                  (UL(0))
#define MVFR0_EL1_UNKN                                  (UL(0))

#define REG_MVFR1_EL1                                   S3_0_C0_C3_1
#define SYS_MVFR1_EL1                                   sys_reg(3, 0, 0, 3, 1)
#define SYS_MVFR1_EL1_Op0                               3
#define SYS_MVFR1_EL1_Op1                               0
#define SYS_MVFR1_EL1_CRn                               0
#define SYS_MVFR1_EL1_CRm                               3
#define SYS_MVFR1_EL1_Op2                               1

#define MVFR1_EL1_SIMDFMAC                              GENMASK(31, 28)
#define MVFR1_EL1_SIMDFMAC_MASK                         GENMASK(31, 28)
#define MVFR1_EL1_SIMDFMAC_SHIFT                        28
#define MVFR1_EL1_SIMDFMAC_WIDTH                        4
#define MVFR1_EL1_SIMDFMAC_SIGNED                       false
#define MVFR1_EL1_SIMDFMAC_NI                           UL(0b0000)
#define MVFR1_EL1_SIMDFMAC_IMP                          UL(0b0001)

#define MVFR1_EL1_FPHP                                  GENMASK(27, 24)
#define MVFR1_EL1_FPHP_MASK                             GENMASK(27, 24)
#define MVFR1_EL1_FPHP_SHIFT                            24
#define MVFR1_EL1_FPHP_WIDTH                            4
#define MVFR1_EL1_FPHP_SIGNED                           false
#define MVFR1_EL1_FPHP_NI                               UL(0b0000)
#define MVFR1_EL1_FPHP_FPHP                             UL(0b0001)
#define MVFR1_EL1_FPHP_FPHP_CONV                        UL(0b0010)
#define MVFR1_EL1_FPHP_FP16                             UL(0b0011)

#define MVFR1_EL1_SIMDHP                                GENMASK(23, 20)
#define MVFR1_EL1_SIMDHP_MASK                           GENMASK(23, 20)
#define MVFR1_EL1_SIMDHP_SHIFT                          20
#define MVFR1_EL1_SIMDHP_WIDTH                          4
#define MVFR1_EL1_SIMDHP_SIGNED                         false
#define MVFR1_EL1_SIMDHP_NI                             UL(0b0000)
#define MVFR1_EL1_SIMDHP_SIMDHP                         UL(0b0001)
#define MVFR1_EL1_SIMDHP_SIMDHP_FLOAT                   UL(0b0010)

#define MVFR1_EL1_SIMDSP                                GENMASK(19, 16)
#define MVFR1_EL1_SIMDSP_MASK                           GENMASK(19, 16)
#define MVFR1_EL1_SIMDSP_SHIFT                          16
#define MVFR1_EL1_SIMDSP_WIDTH                          4
#define MVFR1_EL1_SIMDSP_SIGNED                         false
#define MVFR1_EL1_SIMDSP_NI                             UL(0b0000)
#define MVFR1_EL1_SIMDSP_IMP                            UL(0b0001)

#define MVFR1_EL1_SIMDInt                               GENMASK(15, 12)
#define MVFR1_EL1_SIMDInt_MASK                          GENMASK(15, 12)
#define MVFR1_EL1_SIMDInt_SHIFT                         12
#define MVFR1_EL1_SIMDInt_WIDTH                         4
#define MVFR1_EL1_SIMDInt_SIGNED                        false
#define MVFR1_EL1_SIMDInt_NI                            UL(0b0000)
#define MVFR1_EL1_SIMDInt_IMP                           UL(0b0001)

#define MVFR1_EL1_SIMDLS                                GENMASK(11, 8)
#define MVFR1_EL1_SIMDLS_MASK                           GENMASK(11, 8)
#define MVFR1_EL1_SIMDLS_SHIFT                          8
#define MVFR1_EL1_SIMDLS_WIDTH                          4
#define MVFR1_EL1_SIMDLS_SIGNED                         false
#define MVFR1_EL1_SIMDLS_NI                             UL(0b0000)
#define MVFR1_EL1_SIMDLS_IMP                            UL(0b0001)

#define MVFR1_EL1_FPDNaN                                GENMASK(7, 4)
#define MVFR1_EL1_FPDNaN_MASK                           GENMASK(7, 4)
#define MVFR1_EL1_FPDNaN_SHIFT                          4
#define MVFR1_EL1_FPDNaN_WIDTH                          4
#define MVFR1_EL1_FPDNaN_SIGNED                         false
#define MVFR1_EL1_FPDNaN_NI                             UL(0b0000)
#define MVFR1_EL1_FPDNaN_IMP                            UL(0b0001)

#define MVFR1_EL1_FPFtZ                                 GENMASK(3, 0)
#define MVFR1_EL1_FPFtZ_MASK                            GENMASK(3, 0)
#define MVFR1_EL1_FPFtZ_SHIFT                           0
#define MVFR1_EL1_FPFtZ_WIDTH                           4
#define MVFR1_EL1_FPFtZ_SIGNED                          false
#define MVFR1_EL1_FPFtZ_NI                              UL(0b0000)
#define MVFR1_EL1_FPFtZ_IMP                             UL(0b0001)

#define MVFR1_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 32))
#define MVFR1_EL1_RES1                                  (UL(0))
#define MVFR1_EL1_UNKN                                  (UL(0))

#define REG_MVFR2_EL1                                   S3_0_C0_C3_2
#define SYS_MVFR2_EL1                                   sys_reg(3, 0, 0, 3, 2)
#define SYS_MVFR2_EL1_Op0                               3
#define SYS_MVFR2_EL1_Op1                               0
#define SYS_MVFR2_EL1_CRn                               0
#define SYS_MVFR2_EL1_CRm                               3
#define SYS_MVFR2_EL1_Op2                               2

#define MVFR2_EL1_FPMisc                                GENMASK(7, 4)
#define MVFR2_EL1_FPMisc_MASK                           GENMASK(7, 4)
#define MVFR2_EL1_FPMisc_SHIFT                          4
#define MVFR2_EL1_FPMisc_WIDTH                          4
#define MVFR2_EL1_FPMisc_NI                             UL(0b0000)
#define MVFR2_EL1_FPMisc_FP                             UL(0b0001)
#define MVFR2_EL1_FPMisc_FP_DIRECTED_ROUNDING           UL(0b0010)
#define MVFR2_EL1_FPMisc_FP_ROUNDING                    UL(0b0011)
#define MVFR2_EL1_FPMisc_FP_MAX_MIN                     UL(0b0100)

#define MVFR2_EL1_SIMDMisc                              GENMASK(3, 0)
#define MVFR2_EL1_SIMDMisc_MASK                         GENMASK(3, 0)
#define MVFR2_EL1_SIMDMisc_SHIFT                        0
#define MVFR2_EL1_SIMDMisc_WIDTH                        4
#define MVFR2_EL1_SIMDMisc_NI                           UL(0b0000)
#define MVFR2_EL1_SIMDMisc_SIMD_DIRECTED_ROUNDING       UL(0b0001)
#define MVFR2_EL1_SIMDMisc_SIMD_ROUNDING                UL(0b0010)
#define MVFR2_EL1_SIMDMisc_SIMD_MAX_MIN                 UL(0b0011)

#define MVFR2_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 8))
#define MVFR2_EL1_RES1                                  (UL(0))
#define MVFR2_EL1_UNKN                                  (UL(0))

#define REG_ID_PFR2_EL1                                 S3_0_C0_C3_4
#define SYS_ID_PFR2_EL1                                 sys_reg(3, 0, 0, 3, 4)
#define SYS_ID_PFR2_EL1_Op0                             3
#define SYS_ID_PFR2_EL1_Op1                             0
#define SYS_ID_PFR2_EL1_CRn                             0
#define SYS_ID_PFR2_EL1_CRm                             3
#define SYS_ID_PFR2_EL1_Op2                             4

#define ID_PFR2_EL1_RAS_frac                            GENMASK(11, 8)
#define ID_PFR2_EL1_RAS_frac_MASK                       GENMASK(11, 8)
#define ID_PFR2_EL1_RAS_frac_SHIFT                      8
#define ID_PFR2_EL1_RAS_frac_WIDTH                      4
#define ID_PFR2_EL1_RAS_frac_SIGNED                     false
#define ID_PFR2_EL1_RAS_frac_NI                         UL(0b0000)
#define ID_PFR2_EL1_RAS_frac_RASv1p1                    UL(0b0001)

#define ID_PFR2_EL1_SSBS                                GENMASK(7, 4)
#define ID_PFR2_EL1_SSBS_MASK                           GENMASK(7, 4)
#define ID_PFR2_EL1_SSBS_SHIFT                          4
#define ID_PFR2_EL1_SSBS_WIDTH                          4
#define ID_PFR2_EL1_SSBS_SIGNED                         false
#define ID_PFR2_EL1_SSBS_NI                             UL(0b0000)
#define ID_PFR2_EL1_SSBS_IMP                            UL(0b0001)

#define ID_PFR2_EL1_CSV3                                GENMASK(3, 0)
#define ID_PFR2_EL1_CSV3_MASK                           GENMASK(3, 0)
#define ID_PFR2_EL1_CSV3_SHIFT                          0
#define ID_PFR2_EL1_CSV3_WIDTH                          4
#define ID_PFR2_EL1_CSV3_SIGNED                         false
#define ID_PFR2_EL1_CSV3_NI                             UL(0b0000)
#define ID_PFR2_EL1_CSV3_IMP                            UL(0b0001)

#define ID_PFR2_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 12))
#define ID_PFR2_EL1_RES1                                (UL(0))
#define ID_PFR2_EL1_UNKN                                (UL(0))

#define REG_ID_DFR1_EL1                                 S3_0_C0_C3_5
#define SYS_ID_DFR1_EL1                                 sys_reg(3, 0, 0, 3, 5)
#define SYS_ID_DFR1_EL1_Op0                             3
#define SYS_ID_DFR1_EL1_Op1                             0
#define SYS_ID_DFR1_EL1_CRn                             0
#define SYS_ID_DFR1_EL1_CRm                             3
#define SYS_ID_DFR1_EL1_Op2                             5

#define ID_DFR1_EL1_HPMN0                               GENMASK(7, 4)
#define ID_DFR1_EL1_HPMN0_MASK                          GENMASK(7, 4)
#define ID_DFR1_EL1_HPMN0_SHIFT                         4
#define ID_DFR1_EL1_HPMN0_WIDTH                         4
#define ID_DFR1_EL1_HPMN0_SIGNED                        false
#define ID_DFR1_EL1_HPMN0_NI                            UL(0b0000)
#define ID_DFR1_EL1_HPMN0_IMP                           UL(0b0001)

#define ID_DFR1_EL1_MTPMU                               GENMASK(3, 0)
#define ID_DFR1_EL1_MTPMU_MASK                          GENMASK(3, 0)
#define ID_DFR1_EL1_MTPMU_SHIFT                         0
#define ID_DFR1_EL1_MTPMU_WIDTH                         4
#define ID_DFR1_EL1_MTPMU_IMPDEF                        UL(0b0000)
#define ID_DFR1_EL1_MTPMU_IMP                           UL(0b0001)
#define ID_DFR1_EL1_MTPMU_NI                            UL(0b1111)

#define ID_DFR1_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 8))
#define ID_DFR1_EL1_RES1                                (UL(0))
#define ID_DFR1_EL1_UNKN                                (UL(0))

#define REG_ID_MMFR5_EL1                                S3_0_C0_C3_6
#define SYS_ID_MMFR5_EL1                                sys_reg(3, 0, 0, 3, 6)
#define SYS_ID_MMFR5_EL1_Op0                            3
#define SYS_ID_MMFR5_EL1_Op1                            0
#define SYS_ID_MMFR5_EL1_CRn                            0
#define SYS_ID_MMFR5_EL1_CRm                            3
#define SYS_ID_MMFR5_EL1_Op2                            6

#define ID_MMFR5_EL1_nTLBPA                             GENMASK(7, 4)
#define ID_MMFR5_EL1_nTLBPA_MASK                        GENMASK(7, 4)
#define ID_MMFR5_EL1_nTLBPA_SHIFT                       4
#define ID_MMFR5_EL1_nTLBPA_WIDTH                       4
#define ID_MMFR5_EL1_nTLBPA_SIGNED                      false
#define ID_MMFR5_EL1_nTLBPA_NI                          UL(0b0000)
#define ID_MMFR5_EL1_nTLBPA_IMP                         UL(0b0001)

#define ID_MMFR5_EL1_ETS                                GENMASK(3, 0)
#define ID_MMFR5_EL1_ETS_MASK                           GENMASK(3, 0)
#define ID_MMFR5_EL1_ETS_SHIFT                          0
#define ID_MMFR5_EL1_ETS_WIDTH                          4
#define ID_MMFR5_EL1_ETS_SIGNED                         false
#define ID_MMFR5_EL1_ETS_NI                             UL(0b0000)
#define ID_MMFR5_EL1_ETS_IMP                            UL(0b0001)

#define ID_MMFR5_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 8))
#define ID_MMFR5_EL1_RES1                               (UL(0))
#define ID_MMFR5_EL1_UNKN                               (UL(0))

#define REG_ID_AA64PFR0_EL1                             S3_0_C0_C4_0
#define SYS_ID_AA64PFR0_EL1                             sys_reg(3, 0, 0, 4, 0)
#define SYS_ID_AA64PFR0_EL1_Op0                         3
#define SYS_ID_AA64PFR0_EL1_Op1                         0
#define SYS_ID_AA64PFR0_EL1_CRn                         0
#define SYS_ID_AA64PFR0_EL1_CRm                         4
#define SYS_ID_AA64PFR0_EL1_Op2                         0

#define ID_AA64PFR0_EL1_CSV3                            GENMASK(63, 60)
#define ID_AA64PFR0_EL1_CSV3_MASK                       GENMASK(63, 60)
#define ID_AA64PFR0_EL1_CSV3_SHIFT                      60
#define ID_AA64PFR0_EL1_CSV3_WIDTH                      4
#define ID_AA64PFR0_EL1_CSV3_SIGNED                     false
#define ID_AA64PFR0_EL1_CSV3_NI                         UL(0b0000)
#define ID_AA64PFR0_EL1_CSV3_IMP                        UL(0b0001)

#define ID_AA64PFR0_EL1_CSV2                            GENMASK(59, 56)
#define ID_AA64PFR0_EL1_CSV2_MASK                       GENMASK(59, 56)
#define ID_AA64PFR0_EL1_CSV2_SHIFT                      56
#define ID_AA64PFR0_EL1_CSV2_WIDTH                      4
#define ID_AA64PFR0_EL1_CSV2_SIGNED                     false
#define ID_AA64PFR0_EL1_CSV2_NI                         UL(0b0000)
#define ID_AA64PFR0_EL1_CSV2_IMP                        UL(0b0001)
#define ID_AA64PFR0_EL1_CSV2_CSV2_2                     UL(0b0010)
#define ID_AA64PFR0_EL1_CSV2_CSV2_3                     UL(0b0011)

#define ID_AA64PFR0_EL1_RME                             GENMASK(55, 52)
#define ID_AA64PFR0_EL1_RME_MASK                        GENMASK(55, 52)
#define ID_AA64PFR0_EL1_RME_SHIFT                       52
#define ID_AA64PFR0_EL1_RME_WIDTH                       4
#define ID_AA64PFR0_EL1_RME_SIGNED                      false
#define ID_AA64PFR0_EL1_RME_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_RME_IMP                         UL(0b0001)

#define ID_AA64PFR0_EL1_DIT                             GENMASK(51, 48)
#define ID_AA64PFR0_EL1_DIT_MASK                        GENMASK(51, 48)
#define ID_AA64PFR0_EL1_DIT_SHIFT                       48
#define ID_AA64PFR0_EL1_DIT_WIDTH                       4
#define ID_AA64PFR0_EL1_DIT_SIGNED                      false
#define ID_AA64PFR0_EL1_DIT_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_DIT_IMP                         UL(0b0001)

#define ID_AA64PFR0_EL1_AMU                             GENMASK(47, 44)
#define ID_AA64PFR0_EL1_AMU_MASK                        GENMASK(47, 44)
#define ID_AA64PFR0_EL1_AMU_SHIFT                       44
#define ID_AA64PFR0_EL1_AMU_WIDTH                       4
#define ID_AA64PFR0_EL1_AMU_SIGNED                      false
#define ID_AA64PFR0_EL1_AMU_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_AMU_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_AMU_V1P1                        UL(0b0010)

#define ID_AA64PFR0_EL1_MPAM                            GENMASK(43, 40)
#define ID_AA64PFR0_EL1_MPAM_MASK                       GENMASK(43, 40)
#define ID_AA64PFR0_EL1_MPAM_SHIFT                      40
#define ID_AA64PFR0_EL1_MPAM_WIDTH                      4
#define ID_AA64PFR0_EL1_MPAM_SIGNED                     false
#define ID_AA64PFR0_EL1_MPAM_0                          UL(0b0000)
#define ID_AA64PFR0_EL1_MPAM_1                          UL(0b0001)

#define ID_AA64PFR0_EL1_SEL2                            GENMASK(39, 36)
#define ID_AA64PFR0_EL1_SEL2_MASK                       GENMASK(39, 36)
#define ID_AA64PFR0_EL1_SEL2_SHIFT                      36
#define ID_AA64PFR0_EL1_SEL2_WIDTH                      4
#define ID_AA64PFR0_EL1_SEL2_SIGNED                     false
#define ID_AA64PFR0_EL1_SEL2_NI                         UL(0b0000)
#define ID_AA64PFR0_EL1_SEL2_IMP                        UL(0b0001)

#define ID_AA64PFR0_EL1_SVE                             GENMASK(35, 32)
#define ID_AA64PFR0_EL1_SVE_MASK                        GENMASK(35, 32)
#define ID_AA64PFR0_EL1_SVE_SHIFT                       32
#define ID_AA64PFR0_EL1_SVE_WIDTH                       4
#define ID_AA64PFR0_EL1_SVE_SIGNED                      false
#define ID_AA64PFR0_EL1_SVE_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_SVE_IMP                         UL(0b0001)

#define ID_AA64PFR0_EL1_RAS                             GENMASK(31, 28)
#define ID_AA64PFR0_EL1_RAS_MASK                        GENMASK(31, 28)
#define ID_AA64PFR0_EL1_RAS_SHIFT                       28
#define ID_AA64PFR0_EL1_RAS_WIDTH                       4
#define ID_AA64PFR0_EL1_RAS_SIGNED                      false
#define ID_AA64PFR0_EL1_RAS_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_RAS_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_RAS_V1P1                        UL(0b0010)

#define ID_AA64PFR0_EL1_GIC                             GENMASK(27, 24)
#define ID_AA64PFR0_EL1_GIC_MASK                        GENMASK(27, 24)
#define ID_AA64PFR0_EL1_GIC_SHIFT                       24
#define ID_AA64PFR0_EL1_GIC_WIDTH                       4
#define ID_AA64PFR0_EL1_GIC_SIGNED                      false
#define ID_AA64PFR0_EL1_GIC_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_GIC_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_GIC_V4P1                        UL(0b0010)

#define ID_AA64PFR0_EL1_AdvSIMD                         GENMASK(23, 20)
#define ID_AA64PFR0_EL1_AdvSIMD_MASK                    GENMASK(23, 20)
#define ID_AA64PFR0_EL1_AdvSIMD_SHIFT                   20
#define ID_AA64PFR0_EL1_AdvSIMD_WIDTH                   4
#define ID_AA64PFR0_EL1_AdvSIMD_SIGNED                  true
#define ID_AA64PFR0_EL1_AdvSIMD_IMP                     UL(0b0000)
#define ID_AA64PFR0_EL1_AdvSIMD_FP16                    UL(0b0001)
#define ID_AA64PFR0_EL1_AdvSIMD_NI                      UL(0b1111)

#define ID_AA64PFR0_EL1_FP                              GENMASK(19, 16)
#define ID_AA64PFR0_EL1_FP_MASK                         GENMASK(19, 16)
#define ID_AA64PFR0_EL1_FP_SHIFT                        16
#define ID_AA64PFR0_EL1_FP_WIDTH                        4
#define ID_AA64PFR0_EL1_FP_SIGNED                       true
#define ID_AA64PFR0_EL1_FP_IMP                          UL(0b0000)
#define ID_AA64PFR0_EL1_FP_FP16                         UL(0b0001)
#define ID_AA64PFR0_EL1_FP_NI                           UL(0b1111)

#define ID_AA64PFR0_EL1_EL3                             GENMASK(15, 12)
#define ID_AA64PFR0_EL1_EL3_MASK                        GENMASK(15, 12)
#define ID_AA64PFR0_EL1_EL3_SHIFT                       12
#define ID_AA64PFR0_EL1_EL3_WIDTH                       4
#define ID_AA64PFR0_EL1_EL3_SIGNED                      false
#define ID_AA64PFR0_EL1_EL3_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_EL3_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_EL3_AARCH32                     UL(0b0010)

#define ID_AA64PFR0_EL1_EL2                             GENMASK(11, 8)
#define ID_AA64PFR0_EL1_EL2_MASK                        GENMASK(11, 8)
#define ID_AA64PFR0_EL1_EL2_SHIFT                       8
#define ID_AA64PFR0_EL1_EL2_WIDTH                       4
#define ID_AA64PFR0_EL1_EL2_SIGNED                      false
#define ID_AA64PFR0_EL1_EL2_NI                          UL(0b0000)
#define ID_AA64PFR0_EL1_EL2_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_EL2_AARCH32                     UL(0b0010)

#define ID_AA64PFR0_EL1_EL1                             GENMASK(7, 4)
#define ID_AA64PFR0_EL1_EL1_MASK                        GENMASK(7, 4)
#define ID_AA64PFR0_EL1_EL1_SHIFT                       4
#define ID_AA64PFR0_EL1_EL1_WIDTH                       4
#define ID_AA64PFR0_EL1_EL1_SIGNED                      false
#define ID_AA64PFR0_EL1_EL1_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_EL1_AARCH32                     UL(0b0010)

#define ID_AA64PFR0_EL1_EL0                             GENMASK(3, 0)
#define ID_AA64PFR0_EL1_EL0_MASK                        GENMASK(3, 0)
#define ID_AA64PFR0_EL1_EL0_SHIFT                       0
#define ID_AA64PFR0_EL1_EL0_WIDTH                       4
#define ID_AA64PFR0_EL1_EL0_SIGNED                      false
#define ID_AA64PFR0_EL1_EL0_IMP                         UL(0b0001)
#define ID_AA64PFR0_EL1_EL0_AARCH32                     UL(0b0010)

#define ID_AA64PFR0_EL1_RES0                            (UL(0))
#define ID_AA64PFR0_EL1_RES1                            (UL(0))
#define ID_AA64PFR0_EL1_UNKN                            (UL(0))

#define REG_ID_AA64PFR1_EL1                             S3_0_C0_C4_1
#define SYS_ID_AA64PFR1_EL1                             sys_reg(3, 0, 0, 4, 1)
#define SYS_ID_AA64PFR1_EL1_Op0                         3
#define SYS_ID_AA64PFR1_EL1_Op1                         0
#define SYS_ID_AA64PFR1_EL1_CRn                         0
#define SYS_ID_AA64PFR1_EL1_CRm                         4
#define SYS_ID_AA64PFR1_EL1_Op2                         1

#define ID_AA64PFR1_EL1_PFAR                            GENMASK(63, 60)
#define ID_AA64PFR1_EL1_PFAR_MASK                       GENMASK(63, 60)
#define ID_AA64PFR1_EL1_PFAR_SHIFT                      60
#define ID_AA64PFR1_EL1_PFAR_WIDTH                      4
#define ID_AA64PFR1_EL1_PFAR_SIGNED                     false
#define ID_AA64PFR1_EL1_PFAR_NI                         UL(0b0000)
#define ID_AA64PFR1_EL1_PFAR_IMP                        UL(0b0001)

#define ID_AA64PFR1_EL1_DF2                             GENMASK(59, 56)
#define ID_AA64PFR1_EL1_DF2_MASK                        GENMASK(59, 56)
#define ID_AA64PFR1_EL1_DF2_SHIFT                       56
#define ID_AA64PFR1_EL1_DF2_WIDTH                       4
#define ID_AA64PFR1_EL1_DF2_SIGNED                      false
#define ID_AA64PFR1_EL1_DF2_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_DF2_IMP                         UL(0b0001)

#define ID_AA64PFR1_EL1_MTEX                            GENMASK(55, 52)
#define ID_AA64PFR1_EL1_MTEX_MASK                       GENMASK(55, 52)
#define ID_AA64PFR1_EL1_MTEX_SHIFT                      52
#define ID_AA64PFR1_EL1_MTEX_WIDTH                      4
#define ID_AA64PFR1_EL1_MTEX_SIGNED                     false
#define ID_AA64PFR1_EL1_MTEX_MTE                        UL(0b0000)
#define ID_AA64PFR1_EL1_MTEX_MTE4                       UL(0b0001)

#define ID_AA64PFR1_EL1_THE                             GENMASK(51, 48)
#define ID_AA64PFR1_EL1_THE_MASK                        GENMASK(51, 48)
#define ID_AA64PFR1_EL1_THE_SHIFT                       48
#define ID_AA64PFR1_EL1_THE_WIDTH                       4
#define ID_AA64PFR1_EL1_THE_SIGNED                      false
#define ID_AA64PFR1_EL1_THE_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_THE_IMP                         UL(0b0001)

#define ID_AA64PFR1_EL1_GCS                             GENMASK(47, 44)
#define ID_AA64PFR1_EL1_GCS_MASK                        GENMASK(47, 44)
#define ID_AA64PFR1_EL1_GCS_SHIFT                       44
#define ID_AA64PFR1_EL1_GCS_WIDTH                       4
#define ID_AA64PFR1_EL1_GCS_SIGNED                      false
#define ID_AA64PFR1_EL1_GCS_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_GCS_IMP                         UL(0b0001)

#define ID_AA64PFR1_EL1_MTE_frac                        GENMASK(43, 40)
#define ID_AA64PFR1_EL1_MTE_frac_MASK                   GENMASK(43, 40)
#define ID_AA64PFR1_EL1_MTE_frac_SHIFT                  40
#define ID_AA64PFR1_EL1_MTE_frac_WIDTH                  4
#define ID_AA64PFR1_EL1_MTE_frac_ASYNC                  UL(0b0000)
#define ID_AA64PFR1_EL1_MTE_frac_NI                     UL(0b1111)

#define ID_AA64PFR1_EL1_NMI                             GENMASK(39, 36)
#define ID_AA64PFR1_EL1_NMI_MASK                        GENMASK(39, 36)
#define ID_AA64PFR1_EL1_NMI_SHIFT                       36
#define ID_AA64PFR1_EL1_NMI_WIDTH                       4
#define ID_AA64PFR1_EL1_NMI_SIGNED                      false
#define ID_AA64PFR1_EL1_NMI_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_NMI_IMP                         UL(0b0001)

#define ID_AA64PFR1_EL1_CSV2_frac                       GENMASK(35, 32)
#define ID_AA64PFR1_EL1_CSV2_frac_MASK                  GENMASK(35, 32)
#define ID_AA64PFR1_EL1_CSV2_frac_SHIFT                 32
#define ID_AA64PFR1_EL1_CSV2_frac_WIDTH                 4
#define ID_AA64PFR1_EL1_CSV2_frac_SIGNED                false
#define ID_AA64PFR1_EL1_CSV2_frac_NI                    UL(0b0000)
#define ID_AA64PFR1_EL1_CSV2_frac_CSV2_1p1              UL(0b0001)
#define ID_AA64PFR1_EL1_CSV2_frac_CSV2_1p2              UL(0b0010)

#define ID_AA64PFR1_EL1_RNDR_trap                       GENMASK(31, 28)
#define ID_AA64PFR1_EL1_RNDR_trap_MASK                  GENMASK(31, 28)
#define ID_AA64PFR1_EL1_RNDR_trap_SHIFT                 28
#define ID_AA64PFR1_EL1_RNDR_trap_WIDTH                 4
#define ID_AA64PFR1_EL1_RNDR_trap_SIGNED                false
#define ID_AA64PFR1_EL1_RNDR_trap_NI                    UL(0b0000)
#define ID_AA64PFR1_EL1_RNDR_trap_IMP                   UL(0b0001)

#define ID_AA64PFR1_EL1_SME                             GENMASK(27, 24)
#define ID_AA64PFR1_EL1_SME_MASK                        GENMASK(27, 24)
#define ID_AA64PFR1_EL1_SME_SHIFT                       24
#define ID_AA64PFR1_EL1_SME_WIDTH                       4
#define ID_AA64PFR1_EL1_SME_SIGNED                      false
#define ID_AA64PFR1_EL1_SME_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_SME_IMP                         UL(0b0001)
#define ID_AA64PFR1_EL1_SME_SME2                        UL(0b0010)

#define ID_AA64PFR1_EL1_MPAM_frac                       GENMASK(19, 16)
#define ID_AA64PFR1_EL1_MPAM_frac_MASK                  GENMASK(19, 16)
#define ID_AA64PFR1_EL1_MPAM_frac_SHIFT                 16
#define ID_AA64PFR1_EL1_MPAM_frac_WIDTH                 4
#define ID_AA64PFR1_EL1_MPAM_frac_SIGNED                false
#define ID_AA64PFR1_EL1_MPAM_frac_MINOR_0               UL(0b0000)
#define ID_AA64PFR1_EL1_MPAM_frac_MINOR_1               UL(0b0001)

#define ID_AA64PFR1_EL1_RAS_frac                        GENMASK(15, 12)
#define ID_AA64PFR1_EL1_RAS_frac_MASK                   GENMASK(15, 12)
#define ID_AA64PFR1_EL1_RAS_frac_SHIFT                  12
#define ID_AA64PFR1_EL1_RAS_frac_WIDTH                  4
#define ID_AA64PFR1_EL1_RAS_frac_SIGNED                 false
#define ID_AA64PFR1_EL1_RAS_frac_NI                     UL(0b0000)
#define ID_AA64PFR1_EL1_RAS_frac_RASv1p1                UL(0b0001)

#define ID_AA64PFR1_EL1_MTE                             GENMASK(11, 8)
#define ID_AA64PFR1_EL1_MTE_MASK                        GENMASK(11, 8)
#define ID_AA64PFR1_EL1_MTE_SHIFT                       8
#define ID_AA64PFR1_EL1_MTE_WIDTH                       4
#define ID_AA64PFR1_EL1_MTE_SIGNED                      false
#define ID_AA64PFR1_EL1_MTE_NI                          UL(0b0000)
#define ID_AA64PFR1_EL1_MTE_IMP                         UL(0b0001)
#define ID_AA64PFR1_EL1_MTE_MTE2                        UL(0b0010)
#define ID_AA64PFR1_EL1_MTE_MTE3                        UL(0b0011)

#define ID_AA64PFR1_EL1_SSBS                            GENMASK(7, 4)
#define ID_AA64PFR1_EL1_SSBS_MASK                       GENMASK(7, 4)
#define ID_AA64PFR1_EL1_SSBS_SHIFT                      4
#define ID_AA64PFR1_EL1_SSBS_WIDTH                      4
#define ID_AA64PFR1_EL1_SSBS_SIGNED                     false
#define ID_AA64PFR1_EL1_SSBS_NI                         UL(0b0000)
#define ID_AA64PFR1_EL1_SSBS_IMP                        UL(0b0001)
#define ID_AA64PFR1_EL1_SSBS_SSBS2                      UL(0b0010)

#define ID_AA64PFR1_EL1_BT                              GENMASK(3, 0)
#define ID_AA64PFR1_EL1_BT_MASK                         GENMASK(3, 0)
#define ID_AA64PFR1_EL1_BT_SHIFT                        0
#define ID_AA64PFR1_EL1_BT_WIDTH                        4
#define ID_AA64PFR1_EL1_BT_SIGNED                       false
#define ID_AA64PFR1_EL1_BT_NI                           UL(0b0000)
#define ID_AA64PFR1_EL1_BT_IMP                          UL(0b0001)

#define ID_AA64PFR1_EL1_RES0                            (UL(0) | GENMASK_ULL(23, 20))
#define ID_AA64PFR1_EL1_RES1                            (UL(0))
#define ID_AA64PFR1_EL1_UNKN                            (UL(0))

#define REG_ID_AA64ZFR0_EL1                             S3_0_C0_C4_4
#define SYS_ID_AA64ZFR0_EL1                             sys_reg(3, 0, 0, 4, 4)
#define SYS_ID_AA64ZFR0_EL1_Op0                         3
#define SYS_ID_AA64ZFR0_EL1_Op1                         0
#define SYS_ID_AA64ZFR0_EL1_CRn                         0
#define SYS_ID_AA64ZFR0_EL1_CRm                         4
#define SYS_ID_AA64ZFR0_EL1_Op2                         4

#define ID_AA64ZFR0_EL1_F64MM                           GENMASK(59, 56)
#define ID_AA64ZFR0_EL1_F64MM_MASK                      GENMASK(59, 56)
#define ID_AA64ZFR0_EL1_F64MM_SHIFT                     56
#define ID_AA64ZFR0_EL1_F64MM_WIDTH                     4
#define ID_AA64ZFR0_EL1_F64MM_SIGNED                    false
#define ID_AA64ZFR0_EL1_F64MM_NI                        UL(0b0000)
#define ID_AA64ZFR0_EL1_F64MM_IMP                       UL(0b0001)

#define ID_AA64ZFR0_EL1_F32MM                           GENMASK(55, 52)
#define ID_AA64ZFR0_EL1_F32MM_MASK                      GENMASK(55, 52)
#define ID_AA64ZFR0_EL1_F32MM_SHIFT                     52
#define ID_AA64ZFR0_EL1_F32MM_WIDTH                     4
#define ID_AA64ZFR0_EL1_F32MM_SIGNED                    false
#define ID_AA64ZFR0_EL1_F32MM_NI                        UL(0b0000)
#define ID_AA64ZFR0_EL1_F32MM_IMP                       UL(0b0001)

#define ID_AA64ZFR0_EL1_I8MM                            GENMASK(47, 44)
#define ID_AA64ZFR0_EL1_I8MM_MASK                       GENMASK(47, 44)
#define ID_AA64ZFR0_EL1_I8MM_SHIFT                      44
#define ID_AA64ZFR0_EL1_I8MM_WIDTH                      4
#define ID_AA64ZFR0_EL1_I8MM_SIGNED                     false
#define ID_AA64ZFR0_EL1_I8MM_NI                         UL(0b0000)
#define ID_AA64ZFR0_EL1_I8MM_IMP                        UL(0b0001)

#define ID_AA64ZFR0_EL1_SM4                             GENMASK(43, 40)
#define ID_AA64ZFR0_EL1_SM4_MASK                        GENMASK(43, 40)
#define ID_AA64ZFR0_EL1_SM4_SHIFT                       40
#define ID_AA64ZFR0_EL1_SM4_WIDTH                       4
#define ID_AA64ZFR0_EL1_SM4_SIGNED                      false
#define ID_AA64ZFR0_EL1_SM4_NI                          UL(0b0000)
#define ID_AA64ZFR0_EL1_SM4_IMP                         UL(0b0001)

#define ID_AA64ZFR0_EL1_SHA3                            GENMASK(35, 32)
#define ID_AA64ZFR0_EL1_SHA3_MASK                       GENMASK(35, 32)
#define ID_AA64ZFR0_EL1_SHA3_SHIFT                      32
#define ID_AA64ZFR0_EL1_SHA3_WIDTH                      4
#define ID_AA64ZFR0_EL1_SHA3_SIGNED                     false
#define ID_AA64ZFR0_EL1_SHA3_NI                         UL(0b0000)
#define ID_AA64ZFR0_EL1_SHA3_IMP                        UL(0b0001)

#define ID_AA64ZFR0_EL1_BF16                            GENMASK(23, 20)
#define ID_AA64ZFR0_EL1_BF16_MASK                       GENMASK(23, 20)
#define ID_AA64ZFR0_EL1_BF16_SHIFT                      20
#define ID_AA64ZFR0_EL1_BF16_WIDTH                      4
#define ID_AA64ZFR0_EL1_BF16_SIGNED                     false
#define ID_AA64ZFR0_EL1_BF16_NI                         UL(0b0000)
#define ID_AA64ZFR0_EL1_BF16_IMP                        UL(0b0001)
#define ID_AA64ZFR0_EL1_BF16_EBF16                      UL(0b0010)

#define ID_AA64ZFR0_EL1_BitPerm                         GENMASK(19, 16)
#define ID_AA64ZFR0_EL1_BitPerm_MASK                    GENMASK(19, 16)
#define ID_AA64ZFR0_EL1_BitPerm_SHIFT                   16
#define ID_AA64ZFR0_EL1_BitPerm_WIDTH                   4
#define ID_AA64ZFR0_EL1_BitPerm_SIGNED                  false
#define ID_AA64ZFR0_EL1_BitPerm_NI                      UL(0b0000)
#define ID_AA64ZFR0_EL1_BitPerm_IMP                     UL(0b0001)

#define ID_AA64ZFR0_EL1_AES                             GENMASK(7, 4)
#define ID_AA64ZFR0_EL1_AES_MASK                        GENMASK(7, 4)
#define ID_AA64ZFR0_EL1_AES_SHIFT                       4
#define ID_AA64ZFR0_EL1_AES_WIDTH                       4
#define ID_AA64ZFR0_EL1_AES_SIGNED                      false
#define ID_AA64ZFR0_EL1_AES_NI                          UL(0b0000)
#define ID_AA64ZFR0_EL1_AES_IMP                         UL(0b0001)
#define ID_AA64ZFR0_EL1_AES_PMULL128                    UL(0b0010)

#define ID_AA64ZFR0_EL1_SVEver                          GENMASK(3, 0)
#define ID_AA64ZFR0_EL1_SVEver_MASK                     GENMASK(3, 0)
#define ID_AA64ZFR0_EL1_SVEver_SHIFT                    0
#define ID_AA64ZFR0_EL1_SVEver_WIDTH                    4
#define ID_AA64ZFR0_EL1_SVEver_SIGNED                   false
#define ID_AA64ZFR0_EL1_SVEver_IMP                      UL(0b0000)
#define ID_AA64ZFR0_EL1_SVEver_SVE2                     UL(0b0001)
#define ID_AA64ZFR0_EL1_SVEver_SVE2p1                   UL(0b0010)

#define ID_AA64ZFR0_EL1_RES0                            (UL(0) | GENMASK_ULL(63, 60) | GENMASK_ULL(51, 48) | GENMASK_ULL(39, 36) | GENMASK_ULL(31, 24) | GENMASK_ULL(15, 8))
#define ID_AA64ZFR0_EL1_RES1                            (UL(0))
#define ID_AA64ZFR0_EL1_UNKN                            (UL(0))

#define REG_ID_AA64SMFR0_EL1                            S3_0_C0_C4_5
#define SYS_ID_AA64SMFR0_EL1                            sys_reg(3, 0, 0, 4, 5)
#define SYS_ID_AA64SMFR0_EL1_Op0                        3
#define SYS_ID_AA64SMFR0_EL1_Op1                        0
#define SYS_ID_AA64SMFR0_EL1_CRn                        0
#define SYS_ID_AA64SMFR0_EL1_CRm                        4
#define SYS_ID_AA64SMFR0_EL1_Op2                        5

#define ID_AA64SMFR0_EL1_FA64                           GENMASK(63, 63)
#define ID_AA64SMFR0_EL1_FA64_MASK                      GENMASK(63, 63)
#define ID_AA64SMFR0_EL1_FA64_SHIFT                     63
#define ID_AA64SMFR0_EL1_FA64_WIDTH                     1
#define ID_AA64SMFR0_EL1_FA64_SIGNED                    false
#define ID_AA64SMFR0_EL1_FA64_NI                        UL(0b0)
#define ID_AA64SMFR0_EL1_FA64_IMP                       UL(0b1)

#define ID_AA64SMFR0_EL1_SMEver                         GENMASK(59, 56)
#define ID_AA64SMFR0_EL1_SMEver_MASK                    GENMASK(59, 56)
#define ID_AA64SMFR0_EL1_SMEver_SHIFT                   56
#define ID_AA64SMFR0_EL1_SMEver_WIDTH                   4
#define ID_AA64SMFR0_EL1_SMEver_SIGNED                  false
#define ID_AA64SMFR0_EL1_SMEver_SME                     UL(0b0000)
#define ID_AA64SMFR0_EL1_SMEver_SME2                    UL(0b0001)
#define ID_AA64SMFR0_EL1_SMEver_SME2p1                  UL(0b0010)
#define ID_AA64SMFR0_EL1_SMEver_IMP                     UL(0b0000)

#define ID_AA64SMFR0_EL1_I16I64                         GENMASK(55, 52)
#define ID_AA64SMFR0_EL1_I16I64_MASK                    GENMASK(55, 52)
#define ID_AA64SMFR0_EL1_I16I64_SHIFT                   52
#define ID_AA64SMFR0_EL1_I16I64_WIDTH                   4
#define ID_AA64SMFR0_EL1_I16I64_SIGNED                  false
#define ID_AA64SMFR0_EL1_I16I64_NI                      UL(0b0000)
#define ID_AA64SMFR0_EL1_I16I64_IMP                     UL(0b1111)

#define ID_AA64SMFR0_EL1_F64F64                         GENMASK(48, 48)
#define ID_AA64SMFR0_EL1_F64F64_MASK                    GENMASK(48, 48)
#define ID_AA64SMFR0_EL1_F64F64_SHIFT                   48
#define ID_AA64SMFR0_EL1_F64F64_WIDTH                   1
#define ID_AA64SMFR0_EL1_F64F64_SIGNED                  false
#define ID_AA64SMFR0_EL1_F64F64_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_F64F64_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_I16I32                         GENMASK(47, 44)
#define ID_AA64SMFR0_EL1_I16I32_MASK                    GENMASK(47, 44)
#define ID_AA64SMFR0_EL1_I16I32_SHIFT                   44
#define ID_AA64SMFR0_EL1_I16I32_WIDTH                   4
#define ID_AA64SMFR0_EL1_I16I32_SIGNED                  false
#define ID_AA64SMFR0_EL1_I16I32_NI                      UL(0b0000)
#define ID_AA64SMFR0_EL1_I16I32_IMP                     UL(0b0101)

#define ID_AA64SMFR0_EL1_B16B16                         GENMASK(43, 43)
#define ID_AA64SMFR0_EL1_B16B16_MASK                    GENMASK(43, 43)
#define ID_AA64SMFR0_EL1_B16B16_SHIFT                   43
#define ID_AA64SMFR0_EL1_B16B16_WIDTH                   1
#define ID_AA64SMFR0_EL1_B16B16_SIGNED                  false
#define ID_AA64SMFR0_EL1_B16B16_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_B16B16_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_F16F16                         GENMASK(42, 42)
#define ID_AA64SMFR0_EL1_F16F16_MASK                    GENMASK(42, 42)
#define ID_AA64SMFR0_EL1_F16F16_SHIFT                   42
#define ID_AA64SMFR0_EL1_F16F16_WIDTH                   1
#define ID_AA64SMFR0_EL1_F16F16_SIGNED                  false
#define ID_AA64SMFR0_EL1_F16F16_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_F16F16_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_I8I32                          GENMASK(39, 36)
#define ID_AA64SMFR0_EL1_I8I32_MASK                     GENMASK(39, 36)
#define ID_AA64SMFR0_EL1_I8I32_SHIFT                    36
#define ID_AA64SMFR0_EL1_I8I32_WIDTH                    4
#define ID_AA64SMFR0_EL1_I8I32_SIGNED                   false
#define ID_AA64SMFR0_EL1_I8I32_NI                       UL(0b0000)
#define ID_AA64SMFR0_EL1_I8I32_IMP                      UL(0b1111)

#define ID_AA64SMFR0_EL1_F16F32                         GENMASK(35, 35)
#define ID_AA64SMFR0_EL1_F16F32_MASK                    GENMASK(35, 35)
#define ID_AA64SMFR0_EL1_F16F32_SHIFT                   35
#define ID_AA64SMFR0_EL1_F16F32_WIDTH                   1
#define ID_AA64SMFR0_EL1_F16F32_SIGNED                  false
#define ID_AA64SMFR0_EL1_F16F32_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_F16F32_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_B16F32                         GENMASK(34, 34)
#define ID_AA64SMFR0_EL1_B16F32_MASK                    GENMASK(34, 34)
#define ID_AA64SMFR0_EL1_B16F32_SHIFT                   34
#define ID_AA64SMFR0_EL1_B16F32_WIDTH                   1
#define ID_AA64SMFR0_EL1_B16F32_SIGNED                  false
#define ID_AA64SMFR0_EL1_B16F32_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_B16F32_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_BI32I32                        GENMASK(33, 33)
#define ID_AA64SMFR0_EL1_BI32I32_MASK                   GENMASK(33, 33)
#define ID_AA64SMFR0_EL1_BI32I32_SHIFT                  33
#define ID_AA64SMFR0_EL1_BI32I32_WIDTH                  1
#define ID_AA64SMFR0_EL1_BI32I32_SIGNED                 false
#define ID_AA64SMFR0_EL1_BI32I32_NI                     UL(0b0)
#define ID_AA64SMFR0_EL1_BI32I32_IMP                    UL(0b1)

#define ID_AA64SMFR0_EL1_F32F32                         GENMASK(32, 32)
#define ID_AA64SMFR0_EL1_F32F32_MASK                    GENMASK(32, 32)
#define ID_AA64SMFR0_EL1_F32F32_SHIFT                   32
#define ID_AA64SMFR0_EL1_F32F32_WIDTH                   1
#define ID_AA64SMFR0_EL1_F32F32_SIGNED                  false
#define ID_AA64SMFR0_EL1_F32F32_NI                      UL(0b0)
#define ID_AA64SMFR0_EL1_F32F32_IMP                     UL(0b1)

#define ID_AA64SMFR0_EL1_RES0                           (UL(0) | GENMASK_ULL(62, 60) | GENMASK_ULL(51, 49) | GENMASK_ULL(41, 40) | GENMASK_ULL(31, 0))
#define ID_AA64SMFR0_EL1_RES1                           (UL(0))
#define ID_AA64SMFR0_EL1_UNKN                           (UL(0))

#define REG_ID_AA64DFR0_EL1                             S3_0_C0_C5_0
#define SYS_ID_AA64DFR0_EL1                             sys_reg(3, 0, 0, 5, 0)
#define SYS_ID_AA64DFR0_EL1_Op0                         3
#define SYS_ID_AA64DFR0_EL1_Op1                         0
#define SYS_ID_AA64DFR0_EL1_CRn                         0
#define SYS_ID_AA64DFR0_EL1_CRm                         5
#define SYS_ID_AA64DFR0_EL1_Op2                         0

#define ID_AA64DFR0_EL1_HPMN0                           GENMASK(63, 60)
#define ID_AA64DFR0_EL1_HPMN0_MASK                      GENMASK(63, 60)
#define ID_AA64DFR0_EL1_HPMN0_SHIFT                     60
#define ID_AA64DFR0_EL1_HPMN0_WIDTH                     4
#define ID_AA64DFR0_EL1_HPMN0_UNPREDICTABLE             UL(0b0000)
#define ID_AA64DFR0_EL1_HPMN0_DEF                       UL(0b0001)

#define ID_AA64DFR0_EL1_BRBE                            GENMASK(55, 52)
#define ID_AA64DFR0_EL1_BRBE_MASK                       GENMASK(55, 52)
#define ID_AA64DFR0_EL1_BRBE_SHIFT                      52
#define ID_AA64DFR0_EL1_BRBE_WIDTH                      4
#define ID_AA64DFR0_EL1_BRBE_SIGNED                     false
#define ID_AA64DFR0_EL1_BRBE_NI                         UL(0b0000)
#define ID_AA64DFR0_EL1_BRBE_IMP                        UL(0b0001)
#define ID_AA64DFR0_EL1_BRBE_BRBE_V1P1                  UL(0b0010)

#define ID_AA64DFR0_EL1_MTPMU                           GENMASK(51, 48)
#define ID_AA64DFR0_EL1_MTPMU_MASK                      GENMASK(51, 48)
#define ID_AA64DFR0_EL1_MTPMU_SHIFT                     48
#define ID_AA64DFR0_EL1_MTPMU_WIDTH                     4
#define ID_AA64DFR0_EL1_MTPMU_NI_IMPDEF                 UL(0b0000)
#define ID_AA64DFR0_EL1_MTPMU_IMP                       UL(0b0001)
#define ID_AA64DFR0_EL1_MTPMU_NI                        UL(0b1111)

#define ID_AA64DFR0_EL1_TraceBuffer                     GENMASK(47, 44)
#define ID_AA64DFR0_EL1_TraceBuffer_MASK                GENMASK(47, 44)
#define ID_AA64DFR0_EL1_TraceBuffer_SHIFT               44
#define ID_AA64DFR0_EL1_TraceBuffer_WIDTH               4
#define ID_AA64DFR0_EL1_TraceBuffer_SIGNED              false
#define ID_AA64DFR0_EL1_TraceBuffer_NI                  UL(0b0000)
#define ID_AA64DFR0_EL1_TraceBuffer_IMP                 UL(0b0001)

#define ID_AA64DFR0_EL1_TraceFilt                       GENMASK(43, 40)
#define ID_AA64DFR0_EL1_TraceFilt_MASK                  GENMASK(43, 40)
#define ID_AA64DFR0_EL1_TraceFilt_SHIFT                 40
#define ID_AA64DFR0_EL1_TraceFilt_WIDTH                 4
#define ID_AA64DFR0_EL1_TraceFilt_SIGNED                false
#define ID_AA64DFR0_EL1_TraceFilt_NI                    UL(0b0000)
#define ID_AA64DFR0_EL1_TraceFilt_IMP                   UL(0b0001)

#define ID_AA64DFR0_EL1_DoubleLock                      GENMASK(39, 36)
#define ID_AA64DFR0_EL1_DoubleLock_MASK                 GENMASK(39, 36)
#define ID_AA64DFR0_EL1_DoubleLock_SHIFT                36
#define ID_AA64DFR0_EL1_DoubleLock_WIDTH                4
#define ID_AA64DFR0_EL1_DoubleLock_SIGNED               false
#define ID_AA64DFR0_EL1_DoubleLock_IMP                  UL(0b0000)
#define ID_AA64DFR0_EL1_DoubleLock_NI                   UL(0b1111)

#define ID_AA64DFR0_EL1_PMSVer                          GENMASK(35, 32)
#define ID_AA64DFR0_EL1_PMSVer_MASK                     GENMASK(35, 32)
#define ID_AA64DFR0_EL1_PMSVer_SHIFT                    32
#define ID_AA64DFR0_EL1_PMSVer_WIDTH                    4
#define ID_AA64DFR0_EL1_PMSVer_SIGNED                   false
#define ID_AA64DFR0_EL1_PMSVer_NI                       UL(0b0000)
#define ID_AA64DFR0_EL1_PMSVer_IMP                      UL(0b0001)
#define ID_AA64DFR0_EL1_PMSVer_V1P1                     UL(0b0010)
#define ID_AA64DFR0_EL1_PMSVer_V1P2                     UL(0b0011)
#define ID_AA64DFR0_EL1_PMSVer_V1P3                     UL(0b0100)

#define ID_AA64DFR0_EL1_CTX_CMPs                        GENMASK(31, 28)
#define ID_AA64DFR0_EL1_CTX_CMPs_MASK                   GENMASK(31, 28)
#define ID_AA64DFR0_EL1_CTX_CMPs_SHIFT                  28
#define ID_AA64DFR0_EL1_CTX_CMPs_WIDTH                  4

#define ID_AA64DFR0_EL1_WRPs                            GENMASK(23, 20)
#define ID_AA64DFR0_EL1_WRPs_MASK                       GENMASK(23, 20)
#define ID_AA64DFR0_EL1_WRPs_SHIFT                      20
#define ID_AA64DFR0_EL1_WRPs_WIDTH                      4

#define ID_AA64DFR0_EL1_BRPs                            GENMASK(15, 12)
#define ID_AA64DFR0_EL1_BRPs_MASK                       GENMASK(15, 12)
#define ID_AA64DFR0_EL1_BRPs_SHIFT                      12
#define ID_AA64DFR0_EL1_BRPs_WIDTH                      4

#define ID_AA64DFR0_EL1_PMUVer                          GENMASK(11, 8)
#define ID_AA64DFR0_EL1_PMUVer_MASK                     GENMASK(11, 8)
#define ID_AA64DFR0_EL1_PMUVer_SHIFT                    8
#define ID_AA64DFR0_EL1_PMUVer_WIDTH                    4
#define ID_AA64DFR0_EL1_PMUVer_SIGNED                   false
#define ID_AA64DFR0_EL1_PMUVer_NI                       UL(0b0000)
#define ID_AA64DFR0_EL1_PMUVer_IMP                      UL(0b0001)
#define ID_AA64DFR0_EL1_PMUVer_V3P1                     UL(0b0100)
#define ID_AA64DFR0_EL1_PMUVer_V3P4                     UL(0b0101)
#define ID_AA64DFR0_EL1_PMUVer_V3P5                     UL(0b0110)
#define ID_AA64DFR0_EL1_PMUVer_V3P7                     UL(0b0111)
#define ID_AA64DFR0_EL1_PMUVer_V3P8                     UL(0b1000)
#define ID_AA64DFR0_EL1_PMUVer_IMP_DEF                  UL(0b1111)

#define ID_AA64DFR0_EL1_TraceVer                        GENMASK(7, 4)
#define ID_AA64DFR0_EL1_TraceVer_MASK                   GENMASK(7, 4)
#define ID_AA64DFR0_EL1_TraceVer_SHIFT                  4
#define ID_AA64DFR0_EL1_TraceVer_WIDTH                  4
#define ID_AA64DFR0_EL1_TraceVer_SIGNED                 false
#define ID_AA64DFR0_EL1_TraceVer_NI                     UL(0b0000)
#define ID_AA64DFR0_EL1_TraceVer_IMP                    UL(0b0001)

#define ID_AA64DFR0_EL1_DebugVer                        GENMASK(3, 0)
#define ID_AA64DFR0_EL1_DebugVer_MASK                   GENMASK(3, 0)
#define ID_AA64DFR0_EL1_DebugVer_SHIFT                  0
#define ID_AA64DFR0_EL1_DebugVer_WIDTH                  4
#define ID_AA64DFR0_EL1_DebugVer_SIGNED                 false
#define ID_AA64DFR0_EL1_DebugVer_IMP                    UL(0b0110)
#define ID_AA64DFR0_EL1_DebugVer_VHE                    UL(0b0111)
#define ID_AA64DFR0_EL1_DebugVer_V8P2                   UL(0b1000)
#define ID_AA64DFR0_EL1_DebugVer_V8P4                   UL(0b1001)
#define ID_AA64DFR0_EL1_DebugVer_V8P8                   UL(0b1010)

#define ID_AA64DFR0_EL1_RES0                            (UL(0) | GENMASK_ULL(59, 56) | GENMASK_ULL(27, 24) | GENMASK_ULL(19, 16))
#define ID_AA64DFR0_EL1_RES1                            (UL(0))
#define ID_AA64DFR0_EL1_UNKN                            (UL(0))

#define REG_ID_AA64DFR1_EL1                             S3_0_C0_C5_1
#define SYS_ID_AA64DFR1_EL1                             sys_reg(3, 0, 0, 5, 1)
#define SYS_ID_AA64DFR1_EL1_Op0                         3
#define SYS_ID_AA64DFR1_EL1_Op1                         0
#define SYS_ID_AA64DFR1_EL1_CRn                         0
#define SYS_ID_AA64DFR1_EL1_CRm                         5
#define SYS_ID_AA64DFR1_EL1_Op2                         1

#define ID_AA64DFR1_EL1_RES0                            (UL(0) | GENMASK_ULL(63, 0))
#define ID_AA64DFR1_EL1_RES1                            (UL(0))
#define ID_AA64DFR1_EL1_UNKN                            (UL(0))

#define REG_ID_AA64AFR0_EL1                             S3_0_C0_C5_4
#define SYS_ID_AA64AFR0_EL1                             sys_reg(3, 0, 0, 5, 4)
#define SYS_ID_AA64AFR0_EL1_Op0                         3
#define SYS_ID_AA64AFR0_EL1_Op1                         0
#define SYS_ID_AA64AFR0_EL1_CRn                         0
#define SYS_ID_AA64AFR0_EL1_CRm                         5
#define SYS_ID_AA64AFR0_EL1_Op2                         4

#define ID_AA64AFR0_EL1_IMPDEF7                         GENMASK(31, 28)
#define ID_AA64AFR0_EL1_IMPDEF7_MASK                    GENMASK(31, 28)
#define ID_AA64AFR0_EL1_IMPDEF7_SHIFT                   28
#define ID_AA64AFR0_EL1_IMPDEF7_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF6                         GENMASK(27, 24)
#define ID_AA64AFR0_EL1_IMPDEF6_MASK                    GENMASK(27, 24)
#define ID_AA64AFR0_EL1_IMPDEF6_SHIFT                   24
#define ID_AA64AFR0_EL1_IMPDEF6_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF5                         GENMASK(23, 20)
#define ID_AA64AFR0_EL1_IMPDEF5_MASK                    GENMASK(23, 20)
#define ID_AA64AFR0_EL1_IMPDEF5_SHIFT                   20
#define ID_AA64AFR0_EL1_IMPDEF5_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF4                         GENMASK(19, 16)
#define ID_AA64AFR0_EL1_IMPDEF4_MASK                    GENMASK(19, 16)
#define ID_AA64AFR0_EL1_IMPDEF4_SHIFT                   16
#define ID_AA64AFR0_EL1_IMPDEF4_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF3                         GENMASK(15, 12)
#define ID_AA64AFR0_EL1_IMPDEF3_MASK                    GENMASK(15, 12)
#define ID_AA64AFR0_EL1_IMPDEF3_SHIFT                   12
#define ID_AA64AFR0_EL1_IMPDEF3_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF2                         GENMASK(11, 8)
#define ID_AA64AFR0_EL1_IMPDEF2_MASK                    GENMASK(11, 8)
#define ID_AA64AFR0_EL1_IMPDEF2_SHIFT                   8
#define ID_AA64AFR0_EL1_IMPDEF2_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF1                         GENMASK(7, 4)
#define ID_AA64AFR0_EL1_IMPDEF1_MASK                    GENMASK(7, 4)
#define ID_AA64AFR0_EL1_IMPDEF1_SHIFT                   4
#define ID_AA64AFR0_EL1_IMPDEF1_WIDTH                   4

#define ID_AA64AFR0_EL1_IMPDEF0                         GENMASK(3, 0)
#define ID_AA64AFR0_EL1_IMPDEF0_MASK                    GENMASK(3, 0)
#define ID_AA64AFR0_EL1_IMPDEF0_SHIFT                   0
#define ID_AA64AFR0_EL1_IMPDEF0_WIDTH                   4

#define ID_AA64AFR0_EL1_RES0                            (UL(0) | GENMASK_ULL(63, 32))
#define ID_AA64AFR0_EL1_RES1                            (UL(0))
#define ID_AA64AFR0_EL1_UNKN                            (UL(0))

#define REG_ID_AA64AFR1_EL1                             S3_0_C0_C5_5
#define SYS_ID_AA64AFR1_EL1                             sys_reg(3, 0, 0, 5, 5)
#define SYS_ID_AA64AFR1_EL1_Op0                         3
#define SYS_ID_AA64AFR1_EL1_Op1                         0
#define SYS_ID_AA64AFR1_EL1_CRn                         0
#define SYS_ID_AA64AFR1_EL1_CRm                         5
#define SYS_ID_AA64AFR1_EL1_Op2                         5

#define ID_AA64AFR1_EL1_RES0                            (UL(0) | GENMASK_ULL(63, 0))
#define ID_AA64AFR1_EL1_RES1                            (UL(0))
#define ID_AA64AFR1_EL1_UNKN                            (UL(0))

#define REG_ID_AA64ISAR0_EL1                            S3_0_C0_C6_0
#define SYS_ID_AA64ISAR0_EL1                            sys_reg(3, 0, 0, 6, 0)
#define SYS_ID_AA64ISAR0_EL1_Op0                        3
#define SYS_ID_AA64ISAR0_EL1_Op1                        0
#define SYS_ID_AA64ISAR0_EL1_CRn                        0
#define SYS_ID_AA64ISAR0_EL1_CRm                        6
#define SYS_ID_AA64ISAR0_EL1_Op2                        0

#define ID_AA64ISAR0_EL1_RNDR                           GENMASK(63, 60)
#define ID_AA64ISAR0_EL1_RNDR_MASK                      GENMASK(63, 60)
#define ID_AA64ISAR0_EL1_RNDR_SHIFT                     60
#define ID_AA64ISAR0_EL1_RNDR_WIDTH                     4
#define ID_AA64ISAR0_EL1_RNDR_SIGNED                    false
#define ID_AA64ISAR0_EL1_RNDR_NI                        UL(0b0000)
#define ID_AA64ISAR0_EL1_RNDR_IMP                       UL(0b0001)

#define ID_AA64ISAR0_EL1_TLB                            GENMASK(59, 56)
#define ID_AA64ISAR0_EL1_TLB_MASK                       GENMASK(59, 56)
#define ID_AA64ISAR0_EL1_TLB_SHIFT                      56
#define ID_AA64ISAR0_EL1_TLB_WIDTH                      4
#define ID_AA64ISAR0_EL1_TLB_SIGNED                     false
#define ID_AA64ISAR0_EL1_TLB_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_TLB_OS                         UL(0b0001)
#define ID_AA64ISAR0_EL1_TLB_RANGE                      UL(0b0010)

#define ID_AA64ISAR0_EL1_TS                             GENMASK(55, 52)
#define ID_AA64ISAR0_EL1_TS_MASK                        GENMASK(55, 52)
#define ID_AA64ISAR0_EL1_TS_SHIFT                       52
#define ID_AA64ISAR0_EL1_TS_WIDTH                       4
#define ID_AA64ISAR0_EL1_TS_SIGNED                      false
#define ID_AA64ISAR0_EL1_TS_NI                          UL(0b0000)
#define ID_AA64ISAR0_EL1_TS_FLAGM                       UL(0b0001)
#define ID_AA64ISAR0_EL1_TS_FLAGM2                      UL(0b0010)

#define ID_AA64ISAR0_EL1_FHM                            GENMASK(51, 48)
#define ID_AA64ISAR0_EL1_FHM_MASK                       GENMASK(51, 48)
#define ID_AA64ISAR0_EL1_FHM_SHIFT                      48
#define ID_AA64ISAR0_EL1_FHM_WIDTH                      4
#define ID_AA64ISAR0_EL1_FHM_SIGNED                     false
#define ID_AA64ISAR0_EL1_FHM_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_FHM_IMP                        UL(0b0001)

#define ID_AA64ISAR0_EL1_DP                             GENMASK(47, 44)
#define ID_AA64ISAR0_EL1_DP_MASK                        GENMASK(47, 44)
#define ID_AA64ISAR0_EL1_DP_SHIFT                       44
#define ID_AA64ISAR0_EL1_DP_WIDTH                       4
#define ID_AA64ISAR0_EL1_DP_SIGNED                      false
#define ID_AA64ISAR0_EL1_DP_NI                          UL(0b0000)
#define ID_AA64ISAR0_EL1_DP_IMP                         UL(0b0001)

#define ID_AA64ISAR0_EL1_SM4                            GENMASK(43, 40)
#define ID_AA64ISAR0_EL1_SM4_MASK                       GENMASK(43, 40)
#define ID_AA64ISAR0_EL1_SM4_SHIFT                      40
#define ID_AA64ISAR0_EL1_SM4_WIDTH                      4
#define ID_AA64ISAR0_EL1_SM4_SIGNED                     false
#define ID_AA64ISAR0_EL1_SM4_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_SM4_IMP                        UL(0b0001)

#define ID_AA64ISAR0_EL1_SM3                            GENMASK(39, 36)
#define ID_AA64ISAR0_EL1_SM3_MASK                       GENMASK(39, 36)
#define ID_AA64ISAR0_EL1_SM3_SHIFT                      36
#define ID_AA64ISAR0_EL1_SM3_WIDTH                      4
#define ID_AA64ISAR0_EL1_SM3_SIGNED                     false
#define ID_AA64ISAR0_EL1_SM3_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_SM3_IMP                        UL(0b0001)

#define ID_AA64ISAR0_EL1_SHA3                           GENMASK(35, 32)
#define ID_AA64ISAR0_EL1_SHA3_MASK                      GENMASK(35, 32)
#define ID_AA64ISAR0_EL1_SHA3_SHIFT                     32
#define ID_AA64ISAR0_EL1_SHA3_WIDTH                     4
#define ID_AA64ISAR0_EL1_SHA3_SIGNED                    false
#define ID_AA64ISAR0_EL1_SHA3_NI                        UL(0b0000)
#define ID_AA64ISAR0_EL1_SHA3_IMP                       UL(0b0001)

#define ID_AA64ISAR0_EL1_RDM                            GENMASK(31, 28)
#define ID_AA64ISAR0_EL1_RDM_MASK                       GENMASK(31, 28)
#define ID_AA64ISAR0_EL1_RDM_SHIFT                      28
#define ID_AA64ISAR0_EL1_RDM_WIDTH                      4
#define ID_AA64ISAR0_EL1_RDM_SIGNED                     false
#define ID_AA64ISAR0_EL1_RDM_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_RDM_IMP                        UL(0b0001)

#define ID_AA64ISAR0_EL1_TME                            GENMASK(27, 24)
#define ID_AA64ISAR0_EL1_TME_MASK                       GENMASK(27, 24)
#define ID_AA64ISAR0_EL1_TME_SHIFT                      24
#define ID_AA64ISAR0_EL1_TME_WIDTH                      4
#define ID_AA64ISAR0_EL1_TME_SIGNED                     false
#define ID_AA64ISAR0_EL1_TME_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_TME_IMP                        UL(0b0001)

#define ID_AA64ISAR0_EL1_ATOMIC                         GENMASK(23, 20)
#define ID_AA64ISAR0_EL1_ATOMIC_MASK                    GENMASK(23, 20)
#define ID_AA64ISAR0_EL1_ATOMIC_SHIFT                   20
#define ID_AA64ISAR0_EL1_ATOMIC_WIDTH                   4
#define ID_AA64ISAR0_EL1_ATOMIC_SIGNED                  false
#define ID_AA64ISAR0_EL1_ATOMIC_NI                      UL(0b0000)
#define ID_AA64ISAR0_EL1_ATOMIC_IMP                     UL(0b0010)

#define ID_AA64ISAR0_EL1_CRC32                          GENMASK(19, 16)
#define ID_AA64ISAR0_EL1_CRC32_MASK                     GENMASK(19, 16)
#define ID_AA64ISAR0_EL1_CRC32_SHIFT                    16
#define ID_AA64ISAR0_EL1_CRC32_WIDTH                    4
#define ID_AA64ISAR0_EL1_CRC32_SIGNED                   false
#define ID_AA64ISAR0_EL1_CRC32_NI                       UL(0b0000)
#define ID_AA64ISAR0_EL1_CRC32_IMP                      UL(0b0001)

#define ID_AA64ISAR0_EL1_SHA2                           GENMASK(15, 12)
#define ID_AA64ISAR0_EL1_SHA2_MASK                      GENMASK(15, 12)
#define ID_AA64ISAR0_EL1_SHA2_SHIFT                     12
#define ID_AA64ISAR0_EL1_SHA2_WIDTH                     4
#define ID_AA64ISAR0_EL1_SHA2_SIGNED                    false
#define ID_AA64ISAR0_EL1_SHA2_NI                        UL(0b0000)
#define ID_AA64ISAR0_EL1_SHA2_SHA256                    UL(0b0001)
#define ID_AA64ISAR0_EL1_SHA2_SHA512                    UL(0b0010)

#define ID_AA64ISAR0_EL1_SHA1                           GENMASK(11, 8)
#define ID_AA64ISAR0_EL1_SHA1_MASK                      GENMASK(11, 8)
#define ID_AA64ISAR0_EL1_SHA1_SHIFT                     8
#define ID_AA64ISAR0_EL1_SHA1_WIDTH                     4
#define ID_AA64ISAR0_EL1_SHA1_SIGNED                    false
#define ID_AA64ISAR0_EL1_SHA1_NI                        UL(0b0000)
#define ID_AA64ISAR0_EL1_SHA1_IMP                       UL(0b0001)

#define ID_AA64ISAR0_EL1_AES                            GENMASK(7, 4)
#define ID_AA64ISAR0_EL1_AES_MASK                       GENMASK(7, 4)
#define ID_AA64ISAR0_EL1_AES_SHIFT                      4
#define ID_AA64ISAR0_EL1_AES_WIDTH                      4
#define ID_AA64ISAR0_EL1_AES_SIGNED                     false
#define ID_AA64ISAR0_EL1_AES_NI                         UL(0b0000)
#define ID_AA64ISAR0_EL1_AES_AES                        UL(0b0001)
#define ID_AA64ISAR0_EL1_AES_PMULL                      UL(0b0010)

#define ID_AA64ISAR0_EL1_RES0                           (UL(0) | GENMASK_ULL(3, 0))
#define ID_AA64ISAR0_EL1_RES1                           (UL(0))
#define ID_AA64ISAR0_EL1_UNKN                           (UL(0))

#define REG_ID_AA64ISAR1_EL1                            S3_0_C0_C6_1
#define SYS_ID_AA64ISAR1_EL1                            sys_reg(3, 0, 0, 6, 1)
#define SYS_ID_AA64ISAR1_EL1_Op0                        3
#define SYS_ID_AA64ISAR1_EL1_Op1                        0
#define SYS_ID_AA64ISAR1_EL1_CRn                        0
#define SYS_ID_AA64ISAR1_EL1_CRm                        6
#define SYS_ID_AA64ISAR1_EL1_Op2                        1

#define ID_AA64ISAR1_EL1_LS64                           GENMASK(63, 60)
#define ID_AA64ISAR1_EL1_LS64_MASK                      GENMASK(63, 60)
#define ID_AA64ISAR1_EL1_LS64_SHIFT                     60
#define ID_AA64ISAR1_EL1_LS64_WIDTH                     4
#define ID_AA64ISAR1_EL1_LS64_SIGNED                    false
#define ID_AA64ISAR1_EL1_LS64_NI                        UL(0b0000)
#define ID_AA64ISAR1_EL1_LS64_LS64                      UL(0b0001)
#define ID_AA64ISAR1_EL1_LS64_LS64_V                    UL(0b0010)
#define ID_AA64ISAR1_EL1_LS64_LS64_ACCDATA              UL(0b0011)

#define ID_AA64ISAR1_EL1_XS                             GENMASK(59, 56)
#define ID_AA64ISAR1_EL1_XS_MASK                        GENMASK(59, 56)
#define ID_AA64ISAR1_EL1_XS_SHIFT                       56
#define ID_AA64ISAR1_EL1_XS_WIDTH                       4
#define ID_AA64ISAR1_EL1_XS_SIGNED                      false
#define ID_AA64ISAR1_EL1_XS_NI                          UL(0b0000)
#define ID_AA64ISAR1_EL1_XS_IMP                         UL(0b0001)

#define ID_AA64ISAR1_EL1_I8MM                           GENMASK(55, 52)
#define ID_AA64ISAR1_EL1_I8MM_MASK                      GENMASK(55, 52)
#define ID_AA64ISAR1_EL1_I8MM_SHIFT                     52
#define ID_AA64ISAR1_EL1_I8MM_WIDTH                     4
#define ID_AA64ISAR1_EL1_I8MM_SIGNED                    false
#define ID_AA64ISAR1_EL1_I8MM_NI                        UL(0b0000)
#define ID_AA64ISAR1_EL1_I8MM_IMP                       UL(0b0001)

#define ID_AA64ISAR1_EL1_DGH                            GENMASK(51, 48)
#define ID_AA64ISAR1_EL1_DGH_MASK                       GENMASK(51, 48)
#define ID_AA64ISAR1_EL1_DGH_SHIFT                      48
#define ID_AA64ISAR1_EL1_DGH_WIDTH                      4
#define ID_AA64ISAR1_EL1_DGH_SIGNED                     false
#define ID_AA64ISAR1_EL1_DGH_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_DGH_IMP                        UL(0b0001)

#define ID_AA64ISAR1_EL1_BF16                           GENMASK(47, 44)
#define ID_AA64ISAR1_EL1_BF16_MASK                      GENMASK(47, 44)
#define ID_AA64ISAR1_EL1_BF16_SHIFT                     44
#define ID_AA64ISAR1_EL1_BF16_WIDTH                     4
#define ID_AA64ISAR1_EL1_BF16_SIGNED                    false
#define ID_AA64ISAR1_EL1_BF16_NI                        UL(0b0000)
#define ID_AA64ISAR1_EL1_BF16_IMP                       UL(0b0001)
#define ID_AA64ISAR1_EL1_BF16_EBF16                     UL(0b0010)

#define ID_AA64ISAR1_EL1_SPECRES                        GENMASK(43, 40)
#define ID_AA64ISAR1_EL1_SPECRES_MASK                   GENMASK(43, 40)
#define ID_AA64ISAR1_EL1_SPECRES_SHIFT                  40
#define ID_AA64ISAR1_EL1_SPECRES_WIDTH                  4
#define ID_AA64ISAR1_EL1_SPECRES_SIGNED                 false
#define ID_AA64ISAR1_EL1_SPECRES_NI                     UL(0b0000)
#define ID_AA64ISAR1_EL1_SPECRES_IMP                    UL(0b0001)

#define ID_AA64ISAR1_EL1_SB                             GENMASK(39, 36)
#define ID_AA64ISAR1_EL1_SB_MASK                        GENMASK(39, 36)
#define ID_AA64ISAR1_EL1_SB_SHIFT                       36
#define ID_AA64ISAR1_EL1_SB_WIDTH                       4
#define ID_AA64ISAR1_EL1_SB_SIGNED                      false
#define ID_AA64ISAR1_EL1_SB_NI                          UL(0b0000)
#define ID_AA64ISAR1_EL1_SB_IMP                         UL(0b0001)

#define ID_AA64ISAR1_EL1_FRINTTS                        GENMASK(35, 32)
#define ID_AA64ISAR1_EL1_FRINTTS_MASK                   GENMASK(35, 32)
#define ID_AA64ISAR1_EL1_FRINTTS_SHIFT                  32
#define ID_AA64ISAR1_EL1_FRINTTS_WIDTH                  4
#define ID_AA64ISAR1_EL1_FRINTTS_SIGNED                 false
#define ID_AA64ISAR1_EL1_FRINTTS_NI                     UL(0b0000)
#define ID_AA64ISAR1_EL1_FRINTTS_IMP                    UL(0b0001)

#define ID_AA64ISAR1_EL1_GPI                            GENMASK(31, 28)
#define ID_AA64ISAR1_EL1_GPI_MASK                       GENMASK(31, 28)
#define ID_AA64ISAR1_EL1_GPI_SHIFT                      28
#define ID_AA64ISAR1_EL1_GPI_WIDTH                      4
#define ID_AA64ISAR1_EL1_GPI_SIGNED                     false
#define ID_AA64ISAR1_EL1_GPI_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_GPI_IMP                        UL(0b0001)

#define ID_AA64ISAR1_EL1_GPA                            GENMASK(27, 24)
#define ID_AA64ISAR1_EL1_GPA_MASK                       GENMASK(27, 24)
#define ID_AA64ISAR1_EL1_GPA_SHIFT                      24
#define ID_AA64ISAR1_EL1_GPA_WIDTH                      4
#define ID_AA64ISAR1_EL1_GPA_SIGNED                     false
#define ID_AA64ISAR1_EL1_GPA_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_GPA_IMP                        UL(0b0001)

#define ID_AA64ISAR1_EL1_LRCPC                          GENMASK(23, 20)
#define ID_AA64ISAR1_EL1_LRCPC_MASK                     GENMASK(23, 20)
#define ID_AA64ISAR1_EL1_LRCPC_SHIFT                    20
#define ID_AA64ISAR1_EL1_LRCPC_WIDTH                    4
#define ID_AA64ISAR1_EL1_LRCPC_SIGNED                   false
#define ID_AA64ISAR1_EL1_LRCPC_NI                       UL(0b0000)
#define ID_AA64ISAR1_EL1_LRCPC_IMP                      UL(0b0001)
#define ID_AA64ISAR1_EL1_LRCPC_LRCPC2                   UL(0b0010)

#define ID_AA64ISAR1_EL1_FCMA                           GENMASK(19, 16)
#define ID_AA64ISAR1_EL1_FCMA_MASK                      GENMASK(19, 16)
#define ID_AA64ISAR1_EL1_FCMA_SHIFT                     16
#define ID_AA64ISAR1_EL1_FCMA_WIDTH                     4
#define ID_AA64ISAR1_EL1_FCMA_SIGNED                    false
#define ID_AA64ISAR1_EL1_FCMA_NI                        UL(0b0000)
#define ID_AA64ISAR1_EL1_FCMA_IMP                       UL(0b0001)

#define ID_AA64ISAR1_EL1_JSCVT                          GENMASK(15, 12)
#define ID_AA64ISAR1_EL1_JSCVT_MASK                     GENMASK(15, 12)
#define ID_AA64ISAR1_EL1_JSCVT_SHIFT                    12
#define ID_AA64ISAR1_EL1_JSCVT_WIDTH                    4
#define ID_AA64ISAR1_EL1_JSCVT_SIGNED                   false
#define ID_AA64ISAR1_EL1_JSCVT_NI                       UL(0b0000)
#define ID_AA64ISAR1_EL1_JSCVT_IMP                      UL(0b0001)

#define ID_AA64ISAR1_EL1_API                            GENMASK(11, 8)
#define ID_AA64ISAR1_EL1_API_MASK                       GENMASK(11, 8)
#define ID_AA64ISAR1_EL1_API_SHIFT                      8
#define ID_AA64ISAR1_EL1_API_WIDTH                      4
#define ID_AA64ISAR1_EL1_API_SIGNED                     false
#define ID_AA64ISAR1_EL1_API_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_API_PAuth                      UL(0b0001)
#define ID_AA64ISAR1_EL1_API_EPAC                       UL(0b0010)
#define ID_AA64ISAR1_EL1_API_PAuth2                     UL(0b0011)
#define ID_AA64ISAR1_EL1_API_FPAC                       UL(0b0100)
#define ID_AA64ISAR1_EL1_API_FPACCOMBINE                UL(0b0101)

#define ID_AA64ISAR1_EL1_APA                            GENMASK(7, 4)
#define ID_AA64ISAR1_EL1_APA_MASK                       GENMASK(7, 4)
#define ID_AA64ISAR1_EL1_APA_SHIFT                      4
#define ID_AA64ISAR1_EL1_APA_WIDTH                      4
#define ID_AA64ISAR1_EL1_APA_SIGNED                     false
#define ID_AA64ISAR1_EL1_APA_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_APA_PAuth                      UL(0b0001)
#define ID_AA64ISAR1_EL1_APA_EPAC                       UL(0b0010)
#define ID_AA64ISAR1_EL1_APA_PAuth2                     UL(0b0011)
#define ID_AA64ISAR1_EL1_APA_FPAC                       UL(0b0100)
#define ID_AA64ISAR1_EL1_APA_FPACCOMBINE                UL(0b0101)

#define ID_AA64ISAR1_EL1_DPB                            GENMASK(3, 0)
#define ID_AA64ISAR1_EL1_DPB_MASK                       GENMASK(3, 0)
#define ID_AA64ISAR1_EL1_DPB_SHIFT                      0
#define ID_AA64ISAR1_EL1_DPB_WIDTH                      4
#define ID_AA64ISAR1_EL1_DPB_SIGNED                     false
#define ID_AA64ISAR1_EL1_DPB_NI                         UL(0b0000)
#define ID_AA64ISAR1_EL1_DPB_IMP                        UL(0b0001)
#define ID_AA64ISAR1_EL1_DPB_DPB2                       UL(0b0010)

#define ID_AA64ISAR1_EL1_RES0                           (UL(0))
#define ID_AA64ISAR1_EL1_RES1                           (UL(0))
#define ID_AA64ISAR1_EL1_UNKN                           (UL(0))

#define REG_ID_AA64ISAR2_EL1                            S3_0_C0_C6_2
#define SYS_ID_AA64ISAR2_EL1                            sys_reg(3, 0, 0, 6, 2)
#define SYS_ID_AA64ISAR2_EL1_Op0                        3
#define SYS_ID_AA64ISAR2_EL1_Op1                        0
#define SYS_ID_AA64ISAR2_EL1_CRn                        0
#define SYS_ID_AA64ISAR2_EL1_CRm                        6
#define SYS_ID_AA64ISAR2_EL1_Op2                        2

#define ID_AA64ISAR2_EL1_CSSC                           GENMASK(55, 52)
#define ID_AA64ISAR2_EL1_CSSC_MASK                      GENMASK(55, 52)
#define ID_AA64ISAR2_EL1_CSSC_SHIFT                     52
#define ID_AA64ISAR2_EL1_CSSC_WIDTH                     4
#define ID_AA64ISAR2_EL1_CSSC_SIGNED                    false
#define ID_AA64ISAR2_EL1_CSSC_NI                        UL(0b0000)
#define ID_AA64ISAR2_EL1_CSSC_IMP                       UL(0b0001)

#define ID_AA64ISAR2_EL1_RPRFM                          GENMASK(51, 48)
#define ID_AA64ISAR2_EL1_RPRFM_MASK                     GENMASK(51, 48)
#define ID_AA64ISAR2_EL1_RPRFM_SHIFT                    48
#define ID_AA64ISAR2_EL1_RPRFM_WIDTH                    4
#define ID_AA64ISAR2_EL1_RPRFM_SIGNED                   false
#define ID_AA64ISAR2_EL1_RPRFM_NI                       UL(0b0000)
#define ID_AA64ISAR2_EL1_RPRFM_IMP                      UL(0b0001)

#define ID_AA64ISAR2_EL1_CLRBHB                         GENMASK(31, 28)
#define ID_AA64ISAR2_EL1_CLRBHB_MASK                    GENMASK(31, 28)
#define ID_AA64ISAR2_EL1_CLRBHB_SHIFT                   28
#define ID_AA64ISAR2_EL1_CLRBHB_WIDTH                   4
#define ID_AA64ISAR2_EL1_CLRBHB_SIGNED                  false
#define ID_AA64ISAR2_EL1_CLRBHB_NI                      UL(0b0000)
#define ID_AA64ISAR2_EL1_CLRBHB_IMP                     UL(0b0001)

#define ID_AA64ISAR2_EL1_PAC_frac                       GENMASK(27, 24)
#define ID_AA64ISAR2_EL1_PAC_frac_MASK                  GENMASK(27, 24)
#define ID_AA64ISAR2_EL1_PAC_frac_SHIFT                 24
#define ID_AA64ISAR2_EL1_PAC_frac_WIDTH                 4
#define ID_AA64ISAR2_EL1_PAC_frac_SIGNED                false
#define ID_AA64ISAR2_EL1_PAC_frac_NI                    UL(0b0000)
#define ID_AA64ISAR2_EL1_PAC_frac_IMP                   UL(0b0001)

#define ID_AA64ISAR2_EL1_BC                             GENMASK(23, 20)
#define ID_AA64ISAR2_EL1_BC_MASK                        GENMASK(23, 20)
#define ID_AA64ISAR2_EL1_BC_SHIFT                       20
#define ID_AA64ISAR2_EL1_BC_WIDTH                       4
#define ID_AA64ISAR2_EL1_BC_SIGNED                      false
#define ID_AA64ISAR2_EL1_BC_NI                          UL(0b0000)
#define ID_AA64ISAR2_EL1_BC_IMP                         UL(0b0001)

#define ID_AA64ISAR2_EL1_MOPS                           GENMASK(19, 16)
#define ID_AA64ISAR2_EL1_MOPS_MASK                      GENMASK(19, 16)
#define ID_AA64ISAR2_EL1_MOPS_SHIFT                     16
#define ID_AA64ISAR2_EL1_MOPS_WIDTH                     4
#define ID_AA64ISAR2_EL1_MOPS_SIGNED                    false
#define ID_AA64ISAR2_EL1_MOPS_NI                        UL(0b0000)
#define ID_AA64ISAR2_EL1_MOPS_IMP                       UL(0b0001)

#define ID_AA64ISAR2_EL1_APA3                           GENMASK(15, 12)
#define ID_AA64ISAR2_EL1_APA3_MASK                      GENMASK(15, 12)
#define ID_AA64ISAR2_EL1_APA3_SHIFT                     12
#define ID_AA64ISAR2_EL1_APA3_WIDTH                     4
#define ID_AA64ISAR2_EL1_APA3_SIGNED                    false
#define ID_AA64ISAR2_EL1_APA3_NI                        UL(0b0000)
#define ID_AA64ISAR2_EL1_APA3_PAuth                     UL(0b0001)
#define ID_AA64ISAR2_EL1_APA3_EPAC                      UL(0b0010)
#define ID_AA64ISAR2_EL1_APA3_PAuth2                    UL(0b0011)
#define ID_AA64ISAR2_EL1_APA3_FPAC                      UL(0b0100)
#define ID_AA64ISAR2_EL1_APA3_FPACCOMBINE               UL(0b0101)

#define ID_AA64ISAR2_EL1_GPA3                           GENMASK(11, 8)
#define ID_AA64ISAR2_EL1_GPA3_MASK                      GENMASK(11, 8)
#define ID_AA64ISAR2_EL1_GPA3_SHIFT                     8
#define ID_AA64ISAR2_EL1_GPA3_WIDTH                     4
#define ID_AA64ISAR2_EL1_GPA3_SIGNED                    false
#define ID_AA64ISAR2_EL1_GPA3_NI                        UL(0b0000)
#define ID_AA64ISAR2_EL1_GPA3_IMP                       UL(0b0001)

#define ID_AA64ISAR2_EL1_RPRES                          GENMASK(7, 4)
#define ID_AA64ISAR2_EL1_RPRES_MASK                     GENMASK(7, 4)
#define ID_AA64ISAR2_EL1_RPRES_SHIFT                    4
#define ID_AA64ISAR2_EL1_RPRES_WIDTH                    4
#define ID_AA64ISAR2_EL1_RPRES_SIGNED                   false
#define ID_AA64ISAR2_EL1_RPRES_NI                       UL(0b0000)
#define ID_AA64ISAR2_EL1_RPRES_IMP                      UL(0b0001)

#define ID_AA64ISAR2_EL1_WFxT                           GENMASK(3, 0)
#define ID_AA64ISAR2_EL1_WFxT_MASK                      GENMASK(3, 0)
#define ID_AA64ISAR2_EL1_WFxT_SHIFT                     0
#define ID_AA64ISAR2_EL1_WFxT_WIDTH                     4
#define ID_AA64ISAR2_EL1_WFxT_SIGNED                    false
#define ID_AA64ISAR2_EL1_WFxT_NI                        UL(0b0000)
#define ID_AA64ISAR2_EL1_WFxT_IMP                       UL(0b0010)

#define ID_AA64ISAR2_EL1_RES0                           (UL(0) | GENMASK_ULL(63, 56) | GENMASK_ULL(47, 32))
#define ID_AA64ISAR2_EL1_RES1                           (UL(0))
#define ID_AA64ISAR2_EL1_UNKN                           (UL(0))

#define REG_ID_AA64MMFR0_EL1                            S3_0_C0_C7_0
#define SYS_ID_AA64MMFR0_EL1                            sys_reg(3, 0, 0, 7, 0)
#define SYS_ID_AA64MMFR0_EL1_Op0                        3
#define SYS_ID_AA64MMFR0_EL1_Op1                        0
#define SYS_ID_AA64MMFR0_EL1_CRn                        0
#define SYS_ID_AA64MMFR0_EL1_CRm                        7
#define SYS_ID_AA64MMFR0_EL1_Op2                        0

#define ID_AA64MMFR0_EL1_ECV                            GENMASK(63, 60)
#define ID_AA64MMFR0_EL1_ECV_MASK                       GENMASK(63, 60)
#define ID_AA64MMFR0_EL1_ECV_SHIFT                      60
#define ID_AA64MMFR0_EL1_ECV_WIDTH                      4
#define ID_AA64MMFR0_EL1_ECV_SIGNED                     false
#define ID_AA64MMFR0_EL1_ECV_NI                         UL(0b0000)
#define ID_AA64MMFR0_EL1_ECV_IMP                        UL(0b0001)
#define ID_AA64MMFR0_EL1_ECV_CNTPOFF                    UL(0b0010)

#define ID_AA64MMFR0_EL1_FGT                            GENMASK(59, 56)
#define ID_AA64MMFR0_EL1_FGT_MASK                       GENMASK(59, 56)
#define ID_AA64MMFR0_EL1_FGT_SHIFT                      56
#define ID_AA64MMFR0_EL1_FGT_WIDTH                      4
#define ID_AA64MMFR0_EL1_FGT_SIGNED                     false
#define ID_AA64MMFR0_EL1_FGT_NI                         UL(0b0000)
#define ID_AA64MMFR0_EL1_FGT_IMP                        UL(0b0001)

#define ID_AA64MMFR0_EL1_EXS                            GENMASK(47, 44)
#define ID_AA64MMFR0_EL1_EXS_MASK                       GENMASK(47, 44)
#define ID_AA64MMFR0_EL1_EXS_SHIFT                      44
#define ID_AA64MMFR0_EL1_EXS_WIDTH                      4
#define ID_AA64MMFR0_EL1_EXS_SIGNED                     false
#define ID_AA64MMFR0_EL1_EXS_NI                         UL(0b0000)
#define ID_AA64MMFR0_EL1_EXS_IMP                        UL(0b0001)

#define ID_AA64MMFR0_EL1_TGRAN4_2                       GENMASK(43, 40)
#define ID_AA64MMFR0_EL1_TGRAN4_2_MASK                  GENMASK(43, 40)
#define ID_AA64MMFR0_EL1_TGRAN4_2_SHIFT                 40
#define ID_AA64MMFR0_EL1_TGRAN4_2_WIDTH                 4
#define ID_AA64MMFR0_EL1_TGRAN4_2_TGRAN4                UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN4_2_NI                    UL(0b0001)
#define ID_AA64MMFR0_EL1_TGRAN4_2_IMP                   UL(0b0010)
#define ID_AA64MMFR0_EL1_TGRAN4_2_52_BIT                UL(0b0011)

#define ID_AA64MMFR0_EL1_TGRAN64_2                      GENMASK(39, 36)
#define ID_AA64MMFR0_EL1_TGRAN64_2_MASK                 GENMASK(39, 36)
#define ID_AA64MMFR0_EL1_TGRAN64_2_SHIFT                36
#define ID_AA64MMFR0_EL1_TGRAN64_2_WIDTH                4
#define ID_AA64MMFR0_EL1_TGRAN64_2_TGRAN64              UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN64_2_NI                   UL(0b0001)
#define ID_AA64MMFR0_EL1_TGRAN64_2_IMP                  UL(0b0010)

#define ID_AA64MMFR0_EL1_TGRAN16_2                      GENMASK(35, 32)
#define ID_AA64MMFR0_EL1_TGRAN16_2_MASK                 GENMASK(35, 32)
#define ID_AA64MMFR0_EL1_TGRAN16_2_SHIFT                32
#define ID_AA64MMFR0_EL1_TGRAN16_2_WIDTH                4
#define ID_AA64MMFR0_EL1_TGRAN16_2_TGRAN16              UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN16_2_NI                   UL(0b0001)
#define ID_AA64MMFR0_EL1_TGRAN16_2_IMP                  UL(0b0010)
#define ID_AA64MMFR0_EL1_TGRAN16_2_52_BIT               UL(0b0011)

#define ID_AA64MMFR0_EL1_TGRAN4                         GENMASK(31, 28)
#define ID_AA64MMFR0_EL1_TGRAN4_MASK                    GENMASK(31, 28)
#define ID_AA64MMFR0_EL1_TGRAN4_SHIFT                   28
#define ID_AA64MMFR0_EL1_TGRAN4_WIDTH                   4
#define ID_AA64MMFR0_EL1_TGRAN4_IMP                     UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN4_52_BIT                  UL(0b0001)
#define ID_AA64MMFR0_EL1_TGRAN4_NI                      UL(0b1111)

#define ID_AA64MMFR0_EL1_TGRAN64                        GENMASK(27, 24)
#define ID_AA64MMFR0_EL1_TGRAN64_MASK                   GENMASK(27, 24)
#define ID_AA64MMFR0_EL1_TGRAN64_SHIFT                  24
#define ID_AA64MMFR0_EL1_TGRAN64_WIDTH                  4
#define ID_AA64MMFR0_EL1_TGRAN64_IMP                    UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN64_NI                     UL(0b1111)

#define ID_AA64MMFR0_EL1_TGRAN16                        GENMASK(23, 20)
#define ID_AA64MMFR0_EL1_TGRAN16_MASK                   GENMASK(23, 20)
#define ID_AA64MMFR0_EL1_TGRAN16_SHIFT                  20
#define ID_AA64MMFR0_EL1_TGRAN16_WIDTH                  4
#define ID_AA64MMFR0_EL1_TGRAN16_NI                     UL(0b0000)
#define ID_AA64MMFR0_EL1_TGRAN16_IMP                    UL(0b0001)
#define ID_AA64MMFR0_EL1_TGRAN16_52_BIT                 UL(0b0010)

#define ID_AA64MMFR0_EL1_BIGENDEL0                      GENMASK(19, 16)
#define ID_AA64MMFR0_EL1_BIGENDEL0_MASK                 GENMASK(19, 16)
#define ID_AA64MMFR0_EL1_BIGENDEL0_SHIFT                16
#define ID_AA64MMFR0_EL1_BIGENDEL0_WIDTH                4
#define ID_AA64MMFR0_EL1_BIGENDEL0_SIGNED               false
#define ID_AA64MMFR0_EL1_BIGENDEL0_NI                   UL(0b0000)
#define ID_AA64MMFR0_EL1_BIGENDEL0_IMP                  UL(0b0001)

#define ID_AA64MMFR0_EL1_SNSMEM                         GENMASK(15, 12)
#define ID_AA64MMFR0_EL1_SNSMEM_MASK                    GENMASK(15, 12)
#define ID_AA64MMFR0_EL1_SNSMEM_SHIFT                   12
#define ID_AA64MMFR0_EL1_SNSMEM_WIDTH                   4
#define ID_AA64MMFR0_EL1_SNSMEM_SIGNED                  false
#define ID_AA64MMFR0_EL1_SNSMEM_NI                      UL(0b0000)
#define ID_AA64MMFR0_EL1_SNSMEM_IMP                     UL(0b0001)

#define ID_AA64MMFR0_EL1_BIGEND                         GENMASK(11, 8)
#define ID_AA64MMFR0_EL1_BIGEND_MASK                    GENMASK(11, 8)
#define ID_AA64MMFR0_EL1_BIGEND_SHIFT                   8
#define ID_AA64MMFR0_EL1_BIGEND_WIDTH                   4
#define ID_AA64MMFR0_EL1_BIGEND_SIGNED                  false
#define ID_AA64MMFR0_EL1_BIGEND_NI                      UL(0b0000)
#define ID_AA64MMFR0_EL1_BIGEND_IMP                     UL(0b0001)

#define ID_AA64MMFR0_EL1_ASIDBITS                       GENMASK(7, 4)
#define ID_AA64MMFR0_EL1_ASIDBITS_MASK                  GENMASK(7, 4)
#define ID_AA64MMFR0_EL1_ASIDBITS_SHIFT                 4
#define ID_AA64MMFR0_EL1_ASIDBITS_WIDTH                 4
#define ID_AA64MMFR0_EL1_ASIDBITS_8                     UL(0b0000)
#define ID_AA64MMFR0_EL1_ASIDBITS_16                    UL(0b0010)

#define ID_AA64MMFR0_EL1_PARANGE                        GENMASK(3, 0)
#define ID_AA64MMFR0_EL1_PARANGE_MASK                   GENMASK(3, 0)
#define ID_AA64MMFR0_EL1_PARANGE_SHIFT                  0
#define ID_AA64MMFR0_EL1_PARANGE_WIDTH                  4
#define ID_AA64MMFR0_EL1_PARANGE_32                     UL(0b0000)
#define ID_AA64MMFR0_EL1_PARANGE_36                     UL(0b0001)
#define ID_AA64MMFR0_EL1_PARANGE_40                     UL(0b0010)
#define ID_AA64MMFR0_EL1_PARANGE_42                     UL(0b0011)
#define ID_AA64MMFR0_EL1_PARANGE_44                     UL(0b0100)
#define ID_AA64MMFR0_EL1_PARANGE_48                     UL(0b0101)
#define ID_AA64MMFR0_EL1_PARANGE_52                     UL(0b0110)

#define ID_AA64MMFR0_EL1_RES0                           (UL(0) | GENMASK_ULL(55, 48))
#define ID_AA64MMFR0_EL1_RES1                           (UL(0))
#define ID_AA64MMFR0_EL1_UNKN                           (UL(0))

#define REG_ID_AA64MMFR1_EL1                            S3_0_C0_C7_1
#define SYS_ID_AA64MMFR1_EL1                            sys_reg(3, 0, 0, 7, 1)
#define SYS_ID_AA64MMFR1_EL1_Op0                        3
#define SYS_ID_AA64MMFR1_EL1_Op1                        0
#define SYS_ID_AA64MMFR1_EL1_CRn                        0
#define SYS_ID_AA64MMFR1_EL1_CRm                        7
#define SYS_ID_AA64MMFR1_EL1_Op2                        1

#define ID_AA64MMFR1_EL1_ECBHB                          GENMASK(63, 60)
#define ID_AA64MMFR1_EL1_ECBHB_MASK                     GENMASK(63, 60)
#define ID_AA64MMFR1_EL1_ECBHB_SHIFT                    60
#define ID_AA64MMFR1_EL1_ECBHB_WIDTH                    4
#define ID_AA64MMFR1_EL1_ECBHB_SIGNED                   false
#define ID_AA64MMFR1_EL1_ECBHB_NI                       UL(0b0000)
#define ID_AA64MMFR1_EL1_ECBHB_IMP                      UL(0b0001)

#define ID_AA64MMFR1_EL1_CMOW                           GENMASK(59, 56)
#define ID_AA64MMFR1_EL1_CMOW_MASK                      GENMASK(59, 56)
#define ID_AA64MMFR1_EL1_CMOW_SHIFT                     56
#define ID_AA64MMFR1_EL1_CMOW_WIDTH                     4
#define ID_AA64MMFR1_EL1_CMOW_SIGNED                    false
#define ID_AA64MMFR1_EL1_CMOW_NI                        UL(0b0000)
#define ID_AA64MMFR1_EL1_CMOW_IMP                       UL(0b0001)

#define ID_AA64MMFR1_EL1_TIDCP1                         GENMASK(55, 52)
#define ID_AA64MMFR1_EL1_TIDCP1_MASK                    GENMASK(55, 52)
#define ID_AA64MMFR1_EL1_TIDCP1_SHIFT                   52
#define ID_AA64MMFR1_EL1_TIDCP1_WIDTH                   4
#define ID_AA64MMFR1_EL1_TIDCP1_SIGNED                  false
#define ID_AA64MMFR1_EL1_TIDCP1_NI                      UL(0b0000)
#define ID_AA64MMFR1_EL1_TIDCP1_IMP                     UL(0b0001)

#define ID_AA64MMFR1_EL1_nTLBPA                         GENMASK(51, 48)
#define ID_AA64MMFR1_EL1_nTLBPA_MASK                    GENMASK(51, 48)
#define ID_AA64MMFR1_EL1_nTLBPA_SHIFT                   48
#define ID_AA64MMFR1_EL1_nTLBPA_WIDTH                   4
#define ID_AA64MMFR1_EL1_nTLBPA_SIGNED                  false
#define ID_AA64MMFR1_EL1_nTLBPA_NI                      UL(0b0000)
#define ID_AA64MMFR1_EL1_nTLBPA_IMP                     UL(0b0001)

#define ID_AA64MMFR1_EL1_AFP                            GENMASK(47, 44)
#define ID_AA64MMFR1_EL1_AFP_MASK                       GENMASK(47, 44)
#define ID_AA64MMFR1_EL1_AFP_SHIFT                      44
#define ID_AA64MMFR1_EL1_AFP_WIDTH                      4
#define ID_AA64MMFR1_EL1_AFP_SIGNED                     false
#define ID_AA64MMFR1_EL1_AFP_NI                         UL(0b0000)
#define ID_AA64MMFR1_EL1_AFP_IMP                        UL(0b0001)

#define ID_AA64MMFR1_EL1_HCX                            GENMASK(43, 40)
#define ID_AA64MMFR1_EL1_HCX_MASK                       GENMASK(43, 40)
#define ID_AA64MMFR1_EL1_HCX_SHIFT                      40
#define ID_AA64MMFR1_EL1_HCX_WIDTH                      4
#define ID_AA64MMFR1_EL1_HCX_SIGNED                     false
#define ID_AA64MMFR1_EL1_HCX_NI                         UL(0b0000)
#define ID_AA64MMFR1_EL1_HCX_IMP                        UL(0b0001)

#define ID_AA64MMFR1_EL1_ETS                            GENMASK(39, 36)
#define ID_AA64MMFR1_EL1_ETS_MASK                       GENMASK(39, 36)
#define ID_AA64MMFR1_EL1_ETS_SHIFT                      36
#define ID_AA64MMFR1_EL1_ETS_WIDTH                      4
#define ID_AA64MMFR1_EL1_ETS_SIGNED                     false
#define ID_AA64MMFR1_EL1_ETS_NI                         UL(0b0000)
#define ID_AA64MMFR1_EL1_ETS_IMP                        UL(0b0001)

#define ID_AA64MMFR1_EL1_TWED                           GENMASK(35, 32)
#define ID_AA64MMFR1_EL1_TWED_MASK                      GENMASK(35, 32)
#define ID_AA64MMFR1_EL1_TWED_SHIFT                     32
#define ID_AA64MMFR1_EL1_TWED_WIDTH                     4
#define ID_AA64MMFR1_EL1_TWED_SIGNED                    false
#define ID_AA64MMFR1_EL1_TWED_NI                        UL(0b0000)
#define ID_AA64MMFR1_EL1_TWED_IMP                       UL(0b0001)

#define ID_AA64MMFR1_EL1_XNX                            GENMASK(31, 28)
#define ID_AA64MMFR1_EL1_XNX_MASK                       GENMASK(31, 28)
#define ID_AA64MMFR1_EL1_XNX_SHIFT                      28
#define ID_AA64MMFR1_EL1_XNX_WIDTH                      4
#define ID_AA64MMFR1_EL1_XNX_SIGNED                     false
#define ID_AA64MMFR1_EL1_XNX_NI                         UL(0b0000)
#define ID_AA64MMFR1_EL1_XNX_IMP                        UL(0b0001)

#define ID_AA64MMFR1_EL1_SpecSEI                        GENMASK(27, 24)
#define ID_AA64MMFR1_EL1_SpecSEI_MASK                   GENMASK(27, 24)
#define ID_AA64MMFR1_EL1_SpecSEI_SHIFT                  24
#define ID_AA64MMFR1_EL1_SpecSEI_WIDTH                  4
#define ID_AA64MMFR1_EL1_SpecSEI_SIGNED                 false
#define ID_AA64MMFR1_EL1_SpecSEI_NI                     UL(0b0000)
#define ID_AA64MMFR1_EL1_SpecSEI_IMP                    UL(0b0001)

#define ID_AA64MMFR1_EL1_PAN                            GENMASK(23, 20)
#define ID_AA64MMFR1_EL1_PAN_MASK                       GENMASK(23, 20)
#define ID_AA64MMFR1_EL1_PAN_SHIFT                      20
#define ID_AA64MMFR1_EL1_PAN_WIDTH                      4
#define ID_AA64MMFR1_EL1_PAN_SIGNED                     false
#define ID_AA64MMFR1_EL1_PAN_NI                         UL(0b0000)
#define ID_AA64MMFR1_EL1_PAN_IMP                        UL(0b0001)
#define ID_AA64MMFR1_EL1_PAN_PAN2                       UL(0b0010)
#define ID_AA64MMFR1_EL1_PAN_PAN3                       UL(0b0011)

#define ID_AA64MMFR1_EL1_LO                             GENMASK(19, 16)
#define ID_AA64MMFR1_EL1_LO_MASK                        GENMASK(19, 16)
#define ID_AA64MMFR1_EL1_LO_SHIFT                       16
#define ID_AA64MMFR1_EL1_LO_WIDTH                       4
#define ID_AA64MMFR1_EL1_LO_SIGNED                      false
#define ID_AA64MMFR1_EL1_LO_NI                          UL(0b0000)
#define ID_AA64MMFR1_EL1_LO_IMP                         UL(0b0001)

#define ID_AA64MMFR1_EL1_HPDS                           GENMASK(15, 12)
#define ID_AA64MMFR1_EL1_HPDS_MASK                      GENMASK(15, 12)
#define ID_AA64MMFR1_EL1_HPDS_SHIFT                     12
#define ID_AA64MMFR1_EL1_HPDS_WIDTH                     4
#define ID_AA64MMFR1_EL1_HPDS_SIGNED                    false
#define ID_AA64MMFR1_EL1_HPDS_NI                        UL(0b0000)
#define ID_AA64MMFR1_EL1_HPDS_IMP                       UL(0b0001)
#define ID_AA64MMFR1_EL1_HPDS_HPDS2                     UL(0b0010)

#define ID_AA64MMFR1_EL1_VH                             GENMASK(11, 8)
#define ID_AA64MMFR1_EL1_VH_MASK                        GENMASK(11, 8)
#define ID_AA64MMFR1_EL1_VH_SHIFT                       8
#define ID_AA64MMFR1_EL1_VH_WIDTH                       4
#define ID_AA64MMFR1_EL1_VH_SIGNED                      false
#define ID_AA64MMFR1_EL1_VH_NI                          UL(0b0000)
#define ID_AA64MMFR1_EL1_VH_IMP                         UL(0b0001)

#define ID_AA64MMFR1_EL1_VMIDBits                       GENMASK(7, 4)
#define ID_AA64MMFR1_EL1_VMIDBits_MASK                  GENMASK(7, 4)
#define ID_AA64MMFR1_EL1_VMIDBits_SHIFT                 4
#define ID_AA64MMFR1_EL1_VMIDBits_WIDTH                 4
#define ID_AA64MMFR1_EL1_VMIDBits_8                     UL(0b0000)
#define ID_AA64MMFR1_EL1_VMIDBits_16                    UL(0b0010)

#define ID_AA64MMFR1_EL1_HAFDBS                         GENMASK(3, 0)
#define ID_AA64MMFR1_EL1_HAFDBS_MASK                    GENMASK(3, 0)
#define ID_AA64MMFR1_EL1_HAFDBS_SHIFT                   0
#define ID_AA64MMFR1_EL1_HAFDBS_WIDTH                   4
#define ID_AA64MMFR1_EL1_HAFDBS_SIGNED                  false
#define ID_AA64MMFR1_EL1_HAFDBS_NI                      UL(0b0000)
#define ID_AA64MMFR1_EL1_HAFDBS_AF                      UL(0b0001)
#define ID_AA64MMFR1_EL1_HAFDBS_DBM                     UL(0b0010)

#define ID_AA64MMFR1_EL1_RES0                           (UL(0))
#define ID_AA64MMFR1_EL1_RES1                           (UL(0))
#define ID_AA64MMFR1_EL1_UNKN                           (UL(0))

#define REG_ID_AA64MMFR2_EL1                            S3_0_C0_C7_2
#define SYS_ID_AA64MMFR2_EL1                            sys_reg(3, 0, 0, 7, 2)
#define SYS_ID_AA64MMFR2_EL1_Op0                        3
#define SYS_ID_AA64MMFR2_EL1_Op1                        0
#define SYS_ID_AA64MMFR2_EL1_CRn                        0
#define SYS_ID_AA64MMFR2_EL1_CRm                        7
#define SYS_ID_AA64MMFR2_EL1_Op2                        2

#define ID_AA64MMFR2_EL1_E0PD                           GENMASK(63, 60)
#define ID_AA64MMFR2_EL1_E0PD_MASK                      GENMASK(63, 60)
#define ID_AA64MMFR2_EL1_E0PD_SHIFT                     60
#define ID_AA64MMFR2_EL1_E0PD_WIDTH                     4
#define ID_AA64MMFR2_EL1_E0PD_SIGNED                    false
#define ID_AA64MMFR2_EL1_E0PD_NI                        UL(0b0000)
#define ID_AA64MMFR2_EL1_E0PD_IMP                       UL(0b0001)

#define ID_AA64MMFR2_EL1_EVT                            GENMASK(59, 56)
#define ID_AA64MMFR2_EL1_EVT_MASK                       GENMASK(59, 56)
#define ID_AA64MMFR2_EL1_EVT_SHIFT                      56
#define ID_AA64MMFR2_EL1_EVT_WIDTH                      4
#define ID_AA64MMFR2_EL1_EVT_SIGNED                     false
#define ID_AA64MMFR2_EL1_EVT_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_EVT_IMP                        UL(0b0001)
#define ID_AA64MMFR2_EL1_EVT_TTLBxS                     UL(0b0010)

#define ID_AA64MMFR2_EL1_BBM                            GENMASK(55, 52)
#define ID_AA64MMFR2_EL1_BBM_MASK                       GENMASK(55, 52)
#define ID_AA64MMFR2_EL1_BBM_SHIFT                      52
#define ID_AA64MMFR2_EL1_BBM_WIDTH                      4
#define ID_AA64MMFR2_EL1_BBM_SIGNED                     false
#define ID_AA64MMFR2_EL1_BBM_0                          UL(0b0000)
#define ID_AA64MMFR2_EL1_BBM_1                          UL(0b0001)
#define ID_AA64MMFR2_EL1_BBM_2                          UL(0b0010)

#define ID_AA64MMFR2_EL1_TTL                            GENMASK(51, 48)
#define ID_AA64MMFR2_EL1_TTL_MASK                       GENMASK(51, 48)
#define ID_AA64MMFR2_EL1_TTL_SHIFT                      48
#define ID_AA64MMFR2_EL1_TTL_WIDTH                      4
#define ID_AA64MMFR2_EL1_TTL_SIGNED                     false
#define ID_AA64MMFR2_EL1_TTL_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_TTL_IMP                        UL(0b0001)

#define ID_AA64MMFR2_EL1_FWB                            GENMASK(43, 40)
#define ID_AA64MMFR2_EL1_FWB_MASK                       GENMASK(43, 40)
#define ID_AA64MMFR2_EL1_FWB_SHIFT                      40
#define ID_AA64MMFR2_EL1_FWB_WIDTH                      4
#define ID_AA64MMFR2_EL1_FWB_SIGNED                     false
#define ID_AA64MMFR2_EL1_FWB_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_FWB_IMP                        UL(0b0001)

#define ID_AA64MMFR2_EL1_IDS                            GENMASK(39, 36)
#define ID_AA64MMFR2_EL1_IDS_MASK                       GENMASK(39, 36)
#define ID_AA64MMFR2_EL1_IDS_SHIFT                      36
#define ID_AA64MMFR2_EL1_IDS_WIDTH                      4
#define ID_AA64MMFR2_EL1_IDS_0x0                        UL(0b0000)
#define ID_AA64MMFR2_EL1_IDS_0x18                       UL(0b0001)

#define ID_AA64MMFR2_EL1_AT                             GENMASK(35, 32)
#define ID_AA64MMFR2_EL1_AT_MASK                        GENMASK(35, 32)
#define ID_AA64MMFR2_EL1_AT_SHIFT                       32
#define ID_AA64MMFR2_EL1_AT_WIDTH                       4
#define ID_AA64MMFR2_EL1_AT_SIGNED                      false
#define ID_AA64MMFR2_EL1_AT_NI                          UL(0b0000)
#define ID_AA64MMFR2_EL1_AT_IMP                         UL(0b0001)

#define ID_AA64MMFR2_EL1_ST                             GENMASK(31, 28)
#define ID_AA64MMFR2_EL1_ST_MASK                        GENMASK(31, 28)
#define ID_AA64MMFR2_EL1_ST_SHIFT                       28
#define ID_AA64MMFR2_EL1_ST_WIDTH                       4
#define ID_AA64MMFR2_EL1_ST_39                          UL(0b0000)
#define ID_AA64MMFR2_EL1_ST_48_47                       UL(0b0001)

#define ID_AA64MMFR2_EL1_NV                             GENMASK(27, 24)
#define ID_AA64MMFR2_EL1_NV_MASK                        GENMASK(27, 24)
#define ID_AA64MMFR2_EL1_NV_SHIFT                       24
#define ID_AA64MMFR2_EL1_NV_WIDTH                       4
#define ID_AA64MMFR2_EL1_NV_SIGNED                      false
#define ID_AA64MMFR2_EL1_NV_NI                          UL(0b0000)
#define ID_AA64MMFR2_EL1_NV_IMP                         UL(0b0001)
#define ID_AA64MMFR2_EL1_NV_NV2                         UL(0b0010)

#define ID_AA64MMFR2_EL1_CCIDX                          GENMASK(23, 20)
#define ID_AA64MMFR2_EL1_CCIDX_MASK                     GENMASK(23, 20)
#define ID_AA64MMFR2_EL1_CCIDX_SHIFT                    20
#define ID_AA64MMFR2_EL1_CCIDX_WIDTH                    4
#define ID_AA64MMFR2_EL1_CCIDX_32                       UL(0b0000)
#define ID_AA64MMFR2_EL1_CCIDX_64                       UL(0b0001)

#define ID_AA64MMFR2_EL1_VARange                        GENMASK(19, 16)
#define ID_AA64MMFR2_EL1_VARange_MASK                   GENMASK(19, 16)
#define ID_AA64MMFR2_EL1_VARange_SHIFT                  16
#define ID_AA64MMFR2_EL1_VARange_WIDTH                  4
#define ID_AA64MMFR2_EL1_VARange_48                     UL(0b0000)
#define ID_AA64MMFR2_EL1_VARange_52                     UL(0b0001)

#define ID_AA64MMFR2_EL1_IESB                           GENMASK(15, 12)
#define ID_AA64MMFR2_EL1_IESB_MASK                      GENMASK(15, 12)
#define ID_AA64MMFR2_EL1_IESB_SHIFT                     12
#define ID_AA64MMFR2_EL1_IESB_WIDTH                     4
#define ID_AA64MMFR2_EL1_IESB_SIGNED                    false
#define ID_AA64MMFR2_EL1_IESB_NI                        UL(0b0000)
#define ID_AA64MMFR2_EL1_IESB_IMP                       UL(0b0001)

#define ID_AA64MMFR2_EL1_LSM                            GENMASK(11, 8)
#define ID_AA64MMFR2_EL1_LSM_MASK                       GENMASK(11, 8)
#define ID_AA64MMFR2_EL1_LSM_SHIFT                      8
#define ID_AA64MMFR2_EL1_LSM_WIDTH                      4
#define ID_AA64MMFR2_EL1_LSM_SIGNED                     false
#define ID_AA64MMFR2_EL1_LSM_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_LSM_IMP                        UL(0b0001)

#define ID_AA64MMFR2_EL1_UAO                            GENMASK(7, 4)
#define ID_AA64MMFR2_EL1_UAO_MASK                       GENMASK(7, 4)
#define ID_AA64MMFR2_EL1_UAO_SHIFT                      4
#define ID_AA64MMFR2_EL1_UAO_WIDTH                      4
#define ID_AA64MMFR2_EL1_UAO_SIGNED                     false
#define ID_AA64MMFR2_EL1_UAO_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_UAO_IMP                        UL(0b0001)

#define ID_AA64MMFR2_EL1_CnP                            GENMASK(3, 0)
#define ID_AA64MMFR2_EL1_CnP_MASK                       GENMASK(3, 0)
#define ID_AA64MMFR2_EL1_CnP_SHIFT                      0
#define ID_AA64MMFR2_EL1_CnP_WIDTH                      4
#define ID_AA64MMFR2_EL1_CnP_SIGNED                     false
#define ID_AA64MMFR2_EL1_CnP_NI                         UL(0b0000)
#define ID_AA64MMFR2_EL1_CnP_IMP                        UL(0b0001)

#define ID_AA64MMFR2_EL1_RES0                           (UL(0) | GENMASK_ULL(47, 44))
#define ID_AA64MMFR2_EL1_RES1                           (UL(0))
#define ID_AA64MMFR2_EL1_UNKN                           (UL(0))

#define REG_ID_AA64MMFR3_EL1                            S3_0_C0_C7_3
#define SYS_ID_AA64MMFR3_EL1                            sys_reg(3, 0, 0, 7, 3)
#define SYS_ID_AA64MMFR3_EL1_Op0                        3
#define SYS_ID_AA64MMFR3_EL1_Op1                        0
#define SYS_ID_AA64MMFR3_EL1_CRn                        0
#define SYS_ID_AA64MMFR3_EL1_CRm                        7
#define SYS_ID_AA64MMFR3_EL1_Op2                        3

#define ID_AA64MMFR3_EL1_Spec_FPACC                     GENMASK(63, 60)
#define ID_AA64MMFR3_EL1_Spec_FPACC_MASK                GENMASK(63, 60)
#define ID_AA64MMFR3_EL1_Spec_FPACC_SHIFT               60
#define ID_AA64MMFR3_EL1_Spec_FPACC_WIDTH               4
#define ID_AA64MMFR3_EL1_Spec_FPACC_SIGNED              false
#define ID_AA64MMFR3_EL1_Spec_FPACC_NI                  UL(0b0000)
#define ID_AA64MMFR3_EL1_Spec_FPACC_IMP                 UL(0b0001)

#define ID_AA64MMFR3_EL1_ADERR                          GENMASK(59, 56)
#define ID_AA64MMFR3_EL1_ADERR_MASK                     GENMASK(59, 56)
#define ID_AA64MMFR3_EL1_ADERR_SHIFT                    56
#define ID_AA64MMFR3_EL1_ADERR_WIDTH                    4
#define ID_AA64MMFR3_EL1_ADERR_SIGNED                   false
#define ID_AA64MMFR3_EL1_ADERR_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_ADERR_DEV_ASYNC                UL(0b0001)
#define ID_AA64MMFR3_EL1_ADERR_FEAT_ADERR               UL(0b0010)
#define ID_AA64MMFR3_EL1_ADERR_FEAT_ADERR_IND           UL(0b0011)

#define ID_AA64MMFR3_EL1_SDERR                          GENMASK(55, 52)
#define ID_AA64MMFR3_EL1_SDERR_MASK                     GENMASK(55, 52)
#define ID_AA64MMFR3_EL1_SDERR_SHIFT                    52
#define ID_AA64MMFR3_EL1_SDERR_WIDTH                    4
#define ID_AA64MMFR3_EL1_SDERR_SIGNED                   false
#define ID_AA64MMFR3_EL1_SDERR_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_SDERR_DEV_SYNC                 UL(0b0001)
#define ID_AA64MMFR3_EL1_SDERR_FEAT_ADERR               UL(0b0010)
#define ID_AA64MMFR3_EL1_SDERR_FEAT_ADERR_IND           UL(0b0011)

#define ID_AA64MMFR3_EL1_ANERR                          GENMASK(47, 44)
#define ID_AA64MMFR3_EL1_ANERR_MASK                     GENMASK(47, 44)
#define ID_AA64MMFR3_EL1_ANERR_SHIFT                    44
#define ID_AA64MMFR3_EL1_ANERR_WIDTH                    4
#define ID_AA64MMFR3_EL1_ANERR_SIGNED                   false
#define ID_AA64MMFR3_EL1_ANERR_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_ANERR_ASYNC                    UL(0b0001)
#define ID_AA64MMFR3_EL1_ANERR_FEAT_ANERR               UL(0b0010)
#define ID_AA64MMFR3_EL1_ANERR_FEAT_ANERR_IND           UL(0b0011)

#define ID_AA64MMFR3_EL1_SNERR                          GENMASK(43, 40)
#define ID_AA64MMFR3_EL1_SNERR_MASK                     GENMASK(43, 40)
#define ID_AA64MMFR3_EL1_SNERR_SHIFT                    40
#define ID_AA64MMFR3_EL1_SNERR_WIDTH                    4
#define ID_AA64MMFR3_EL1_SNERR_SIGNED                   false
#define ID_AA64MMFR3_EL1_SNERR_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_SNERR_SYNC                     UL(0b0001)
#define ID_AA64MMFR3_EL1_SNERR_FEAT_ANERR               UL(0b0010)
#define ID_AA64MMFR3_EL1_SNERR_FEAT_ANERR_IND           UL(0b0011)

#define ID_AA64MMFR3_EL1_D128_2                         GENMASK(39, 36)
#define ID_AA64MMFR3_EL1_D128_2_MASK                    GENMASK(39, 36)
#define ID_AA64MMFR3_EL1_D128_2_SHIFT                   36
#define ID_AA64MMFR3_EL1_D128_2_WIDTH                   4
#define ID_AA64MMFR3_EL1_D128_2_SIGNED                  false
#define ID_AA64MMFR3_EL1_D128_2_NI                      UL(0b0000)
#define ID_AA64MMFR3_EL1_D128_2_IMP                     UL(0b0001)

#define ID_AA64MMFR3_EL1_D128                           GENMASK(35, 32)
#define ID_AA64MMFR3_EL1_D128_MASK                      GENMASK(35, 32)
#define ID_AA64MMFR3_EL1_D128_SHIFT                     32
#define ID_AA64MMFR3_EL1_D128_WIDTH                     4
#define ID_AA64MMFR3_EL1_D128_SIGNED                    false
#define ID_AA64MMFR3_EL1_D128_NI                        UL(0b0000)
#define ID_AA64MMFR3_EL1_D128_IMP                       UL(0b0001)

#define ID_AA64MMFR3_EL1_MEC                            GENMASK(31, 28)
#define ID_AA64MMFR3_EL1_MEC_MASK                       GENMASK(31, 28)
#define ID_AA64MMFR3_EL1_MEC_SHIFT                      28
#define ID_AA64MMFR3_EL1_MEC_WIDTH                      4
#define ID_AA64MMFR3_EL1_MEC_SIGNED                     false
#define ID_AA64MMFR3_EL1_MEC_NI                         UL(0b0000)
#define ID_AA64MMFR3_EL1_MEC_IMP                        UL(0b0001)

#define ID_AA64MMFR3_EL1_AIE                            GENMASK(27, 24)
#define ID_AA64MMFR3_EL1_AIE_MASK                       GENMASK(27, 24)
#define ID_AA64MMFR3_EL1_AIE_SHIFT                      24
#define ID_AA64MMFR3_EL1_AIE_WIDTH                      4
#define ID_AA64MMFR3_EL1_AIE_SIGNED                     false
#define ID_AA64MMFR3_EL1_AIE_NI                         UL(0b0000)
#define ID_AA64MMFR3_EL1_AIE_IMP                        UL(0b0001)

#define ID_AA64MMFR3_EL1_S2POE                          GENMASK(23, 20)
#define ID_AA64MMFR3_EL1_S2POE_MASK                     GENMASK(23, 20)
#define ID_AA64MMFR3_EL1_S2POE_SHIFT                    20
#define ID_AA64MMFR3_EL1_S2POE_WIDTH                    4
#define ID_AA64MMFR3_EL1_S2POE_SIGNED                   false
#define ID_AA64MMFR3_EL1_S2POE_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_S2POE_IMP                      UL(0b0001)

#define ID_AA64MMFR3_EL1_S1POE                          GENMASK(19, 16)
#define ID_AA64MMFR3_EL1_S1POE_MASK                     GENMASK(19, 16)
#define ID_AA64MMFR3_EL1_S1POE_SHIFT                    16
#define ID_AA64MMFR3_EL1_S1POE_WIDTH                    4
#define ID_AA64MMFR3_EL1_S1POE_SIGNED                   false
#define ID_AA64MMFR3_EL1_S1POE_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_S1POE_IMP                      UL(0b0001)

#define ID_AA64MMFR3_EL1_S2PIE                          GENMASK(15, 12)
#define ID_AA64MMFR3_EL1_S2PIE_MASK                     GENMASK(15, 12)
#define ID_AA64MMFR3_EL1_S2PIE_SHIFT                    12
#define ID_AA64MMFR3_EL1_S2PIE_WIDTH                    4
#define ID_AA64MMFR3_EL1_S2PIE_SIGNED                   false
#define ID_AA64MMFR3_EL1_S2PIE_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_S2PIE_IMP                      UL(0b0001)

#define ID_AA64MMFR3_EL1_S1PIE                          GENMASK(11, 8)
#define ID_AA64MMFR3_EL1_S1PIE_MASK                     GENMASK(11, 8)
#define ID_AA64MMFR3_EL1_S1PIE_SHIFT                    8
#define ID_AA64MMFR3_EL1_S1PIE_WIDTH                    4
#define ID_AA64MMFR3_EL1_S1PIE_SIGNED                   false
#define ID_AA64MMFR3_EL1_S1PIE_NI                       UL(0b0000)
#define ID_AA64MMFR3_EL1_S1PIE_IMP                      UL(0b0001)

#define ID_AA64MMFR3_EL1_SCTLRX                         GENMASK(7, 4)
#define ID_AA64MMFR3_EL1_SCTLRX_MASK                    GENMASK(7, 4)
#define ID_AA64MMFR3_EL1_SCTLRX_SHIFT                   4
#define ID_AA64MMFR3_EL1_SCTLRX_WIDTH                   4
#define ID_AA64MMFR3_EL1_SCTLRX_SIGNED                  false
#define ID_AA64MMFR3_EL1_SCTLRX_NI                      UL(0b0000)
#define ID_AA64MMFR3_EL1_SCTLRX_IMP                     UL(0b0001)

#define ID_AA64MMFR3_EL1_TCRX                           GENMASK(3, 0)
#define ID_AA64MMFR3_EL1_TCRX_MASK                      GENMASK(3, 0)
#define ID_AA64MMFR3_EL1_TCRX_SHIFT                     0
#define ID_AA64MMFR3_EL1_TCRX_WIDTH                     4
#define ID_AA64MMFR3_EL1_TCRX_SIGNED                    false
#define ID_AA64MMFR3_EL1_TCRX_NI                        UL(0b0000)
#define ID_AA64MMFR3_EL1_TCRX_IMP                       UL(0b0001)

#define ID_AA64MMFR3_EL1_RES0                           (UL(0) | GENMASK_ULL(51, 48))
#define ID_AA64MMFR3_EL1_RES1                           (UL(0))
#define ID_AA64MMFR3_EL1_UNKN                           (UL(0))

#define REG_SCTLR_EL1                                   S3_0_C1_C0_0
#define SYS_SCTLR_EL1                                   sys_reg(3, 0, 1, 0, 0)
#define SYS_SCTLR_EL1_Op0                               3
#define SYS_SCTLR_EL1_Op1                               0
#define SYS_SCTLR_EL1_CRn                               1
#define SYS_SCTLR_EL1_CRm                               0
#define SYS_SCTLR_EL1_Op2                               0

#define SCTLR_EL1_TIDCP                                 GENMASK(63, 63)
#define SCTLR_EL1_TIDCP_MASK                            GENMASK(63, 63)
#define SCTLR_EL1_TIDCP_SHIFT                           63
#define SCTLR_EL1_TIDCP_WIDTH                           1

#define SCTLR_EL1_SPINTMASK                             GENMASK(62, 62)
#define SCTLR_EL1_SPINTMASK_MASK                        GENMASK(62, 62)
#define SCTLR_EL1_SPINTMASK_SHIFT                       62
#define SCTLR_EL1_SPINTMASK_WIDTH                       1

#define SCTLR_EL1_NMI                                   GENMASK(61, 61)
#define SCTLR_EL1_NMI_MASK                              GENMASK(61, 61)
#define SCTLR_EL1_NMI_SHIFT                             61
#define SCTLR_EL1_NMI_WIDTH                             1

#define SCTLR_EL1_EnTP2                                 GENMASK(60, 60)
#define SCTLR_EL1_EnTP2_MASK                            GENMASK(60, 60)
#define SCTLR_EL1_EnTP2_SHIFT                           60
#define SCTLR_EL1_EnTP2_WIDTH                           1

#define SCTLR_EL1_EPAN                                  GENMASK(57, 57)
#define SCTLR_EL1_EPAN_MASK                             GENMASK(57, 57)
#define SCTLR_EL1_EPAN_SHIFT                            57
#define SCTLR_EL1_EPAN_WIDTH                            1

#define SCTLR_EL1_EnALS                                 GENMASK(56, 56)
#define SCTLR_EL1_EnALS_MASK                            GENMASK(56, 56)
#define SCTLR_EL1_EnALS_SHIFT                           56
#define SCTLR_EL1_EnALS_WIDTH                           1

#define SCTLR_EL1_EnAS0                                 GENMASK(55, 55)
#define SCTLR_EL1_EnAS0_MASK                            GENMASK(55, 55)
#define SCTLR_EL1_EnAS0_SHIFT                           55
#define SCTLR_EL1_EnAS0_WIDTH                           1

#define SCTLR_EL1_EnASR                                 GENMASK(54, 54)
#define SCTLR_EL1_EnASR_MASK                            GENMASK(54, 54)
#define SCTLR_EL1_EnASR_SHIFT                           54
#define SCTLR_EL1_EnASR_WIDTH                           1

#define SCTLR_EL1_TME                                   GENMASK(53, 53)
#define SCTLR_EL1_TME_MASK                              GENMASK(53, 53)
#define SCTLR_EL1_TME_SHIFT                             53
#define SCTLR_EL1_TME_WIDTH                             1

#define SCTLR_EL1_TME0                                  GENMASK(52, 52)
#define SCTLR_EL1_TME0_MASK                             GENMASK(52, 52)
#define SCTLR_EL1_TME0_SHIFT                            52
#define SCTLR_EL1_TME0_WIDTH                            1

#define SCTLR_EL1_TMT                                   GENMASK(51, 51)
#define SCTLR_EL1_TMT_MASK                              GENMASK(51, 51)
#define SCTLR_EL1_TMT_SHIFT                             51
#define SCTLR_EL1_TMT_WIDTH                             1

#define SCTLR_EL1_TMT0                                  GENMASK(50, 50)
#define SCTLR_EL1_TMT0_MASK                             GENMASK(50, 50)
#define SCTLR_EL1_TMT0_SHIFT                            50
#define SCTLR_EL1_TMT0_WIDTH                            1

#define SCTLR_EL1_TWEDEL                                GENMASK(49, 46)
#define SCTLR_EL1_TWEDEL_MASK                           GENMASK(49, 46)
#define SCTLR_EL1_TWEDEL_SHIFT                          46
#define SCTLR_EL1_TWEDEL_WIDTH                          4

#define SCTLR_EL1_TWEDEn                                GENMASK(45, 45)
#define SCTLR_EL1_TWEDEn_MASK                           GENMASK(45, 45)
#define SCTLR_EL1_TWEDEn_SHIFT                          45
#define SCTLR_EL1_TWEDEn_WIDTH                          1

#define SCTLR_EL1_DSSBS                                 GENMASK(44, 44)
#define SCTLR_EL1_DSSBS_MASK                            GENMASK(44, 44)
#define SCTLR_EL1_DSSBS_SHIFT                           44
#define SCTLR_EL1_DSSBS_WIDTH                           1

#define SCTLR_EL1_ATA                                   GENMASK(43, 43)
#define SCTLR_EL1_ATA_MASK                              GENMASK(43, 43)
#define SCTLR_EL1_ATA_SHIFT                             43
#define SCTLR_EL1_ATA_WIDTH                             1

#define SCTLR_EL1_ATA0                                  GENMASK(42, 42)
#define SCTLR_EL1_ATA0_MASK                             GENMASK(42, 42)
#define SCTLR_EL1_ATA0_SHIFT                            42
#define SCTLR_EL1_ATA0_WIDTH                            1

#define SCTLR_EL1_TCF                                   GENMASK(41, 40)
#define SCTLR_EL1_TCF_MASK                              GENMASK(41, 40)
#define SCTLR_EL1_TCF_SHIFT                             40
#define SCTLR_EL1_TCF_WIDTH                             2
#define SCTLR_EL1_TCF_NONE                              UL(0b00)
#define SCTLR_EL1_TCF_SYNC                              UL(0b01)
#define SCTLR_EL1_TCF_ASYNC                             UL(0b10)
#define SCTLR_EL1_TCF_ASYMM                             UL(0b11)

#define SCTLR_EL1_TCF0                                  GENMASK(39, 38)
#define SCTLR_EL1_TCF0_MASK                             GENMASK(39, 38)
#define SCTLR_EL1_TCF0_SHIFT                            38
#define SCTLR_EL1_TCF0_WIDTH                            2
#define SCTLR_EL1_TCF0_NONE                             UL(0b00)
#define SCTLR_EL1_TCF0_SYNC                             UL(0b01)
#define SCTLR_EL1_TCF0_ASYNC                            UL(0b10)
#define SCTLR_EL1_TCF0_ASYMM                            UL(0b11)

#define SCTLR_EL1_ITFSB                                 GENMASK(37, 37)
#define SCTLR_EL1_ITFSB_MASK                            GENMASK(37, 37)
#define SCTLR_EL1_ITFSB_SHIFT                           37
#define SCTLR_EL1_ITFSB_WIDTH                           1

#define SCTLR_EL1_BT1                                   GENMASK(36, 36)
#define SCTLR_EL1_BT1_MASK                              GENMASK(36, 36)
#define SCTLR_EL1_BT1_SHIFT                             36
#define SCTLR_EL1_BT1_WIDTH                             1

#define SCTLR_EL1_BT0                                   GENMASK(35, 35)
#define SCTLR_EL1_BT0_MASK                              GENMASK(35, 35)
#define SCTLR_EL1_BT0_SHIFT                             35
#define SCTLR_EL1_BT0_WIDTH                             1

#define SCTLR_EL1_MSCEn                                 GENMASK(33, 33)
#define SCTLR_EL1_MSCEn_MASK                            GENMASK(33, 33)
#define SCTLR_EL1_MSCEn_SHIFT                           33
#define SCTLR_EL1_MSCEn_WIDTH                           1

#define SCTLR_EL1_CMOW                                  GENMASK(32, 32)
#define SCTLR_EL1_CMOW_MASK                             GENMASK(32, 32)
#define SCTLR_EL1_CMOW_SHIFT                            32
#define SCTLR_EL1_CMOW_WIDTH                            1

#define SCTLR_EL1_EnIA                                  GENMASK(31, 31)
#define SCTLR_EL1_EnIA_MASK                             GENMASK(31, 31)
#define SCTLR_EL1_EnIA_SHIFT                            31
#define SCTLR_EL1_EnIA_WIDTH                            1

#define SCTLR_EL1_EnIB                                  GENMASK(30, 30)
#define SCTLR_EL1_EnIB_MASK                             GENMASK(30, 30)
#define SCTLR_EL1_EnIB_SHIFT                            30
#define SCTLR_EL1_EnIB_WIDTH                            1

#define SCTLR_EL1_LSMAOE                                GENMASK(29, 29)
#define SCTLR_EL1_LSMAOE_MASK                           GENMASK(29, 29)
#define SCTLR_EL1_LSMAOE_SHIFT                          29
#define SCTLR_EL1_LSMAOE_WIDTH                          1

#define SCTLR_EL1_nTLSMD                                GENMASK(28, 28)
#define SCTLR_EL1_nTLSMD_MASK                           GENMASK(28, 28)
#define SCTLR_EL1_nTLSMD_SHIFT                          28
#define SCTLR_EL1_nTLSMD_WIDTH                          1

#define SCTLR_EL1_EnDA                                  GENMASK(27, 27)
#define SCTLR_EL1_EnDA_MASK                             GENMASK(27, 27)
#define SCTLR_EL1_EnDA_SHIFT                            27
#define SCTLR_EL1_EnDA_WIDTH                            1

#define SCTLR_EL1_UCI                                   GENMASK(26, 26)
#define SCTLR_EL1_UCI_MASK                              GENMASK(26, 26)
#define SCTLR_EL1_UCI_SHIFT                             26
#define SCTLR_EL1_UCI_WIDTH                             1

#define SCTLR_EL1_EE                                    GENMASK(25, 25)
#define SCTLR_EL1_EE_MASK                               GENMASK(25, 25)
#define SCTLR_EL1_EE_SHIFT                              25
#define SCTLR_EL1_EE_WIDTH                              1

#define SCTLR_EL1_E0E                                   GENMASK(24, 24)
#define SCTLR_EL1_E0E_MASK                              GENMASK(24, 24)
#define SCTLR_EL1_E0E_SHIFT                             24
#define SCTLR_EL1_E0E_WIDTH                             1

#define SCTLR_EL1_SPAN                                  GENMASK(23, 23)
#define SCTLR_EL1_SPAN_MASK                             GENMASK(23, 23)
#define SCTLR_EL1_SPAN_SHIFT                            23
#define SCTLR_EL1_SPAN_WIDTH                            1

#define SCTLR_EL1_EIS                                   GENMASK(22, 22)
#define SCTLR_EL1_EIS_MASK                              GENMASK(22, 22)
#define SCTLR_EL1_EIS_SHIFT                             22
#define SCTLR_EL1_EIS_WIDTH                             1

#define SCTLR_EL1_IESB                                  GENMASK(21, 21)
#define SCTLR_EL1_IESB_MASK                             GENMASK(21, 21)
#define SCTLR_EL1_IESB_SHIFT                            21
#define SCTLR_EL1_IESB_WIDTH                            1

#define SCTLR_EL1_TSCXT                                 GENMASK(20, 20)
#define SCTLR_EL1_TSCXT_MASK                            GENMASK(20, 20)
#define SCTLR_EL1_TSCXT_SHIFT                           20
#define SCTLR_EL1_TSCXT_WIDTH                           1

#define SCTLR_EL1_WXN                                   GENMASK(19, 19)
#define SCTLR_EL1_WXN_MASK                              GENMASK(19, 19)
#define SCTLR_EL1_WXN_SHIFT                             19
#define SCTLR_EL1_WXN_WIDTH                             1

#define SCTLR_EL1_nTWE                                  GENMASK(18, 18)
#define SCTLR_EL1_nTWE_MASK                             GENMASK(18, 18)
#define SCTLR_EL1_nTWE_SHIFT                            18
#define SCTLR_EL1_nTWE_WIDTH                            1

#define SCTLR_EL1_nTWI                                  GENMASK(16, 16)
#define SCTLR_EL1_nTWI_MASK                             GENMASK(16, 16)
#define SCTLR_EL1_nTWI_SHIFT                            16
#define SCTLR_EL1_nTWI_WIDTH                            1

#define SCTLR_EL1_UCT                                   GENMASK(15, 15)
#define SCTLR_EL1_UCT_MASK                              GENMASK(15, 15)
#define SCTLR_EL1_UCT_SHIFT                             15
#define SCTLR_EL1_UCT_WIDTH                             1

#define SCTLR_EL1_DZE                                   GENMASK(14, 14)
#define SCTLR_EL1_DZE_MASK                              GENMASK(14, 14)
#define SCTLR_EL1_DZE_SHIFT                             14
#define SCTLR_EL1_DZE_WIDTH                             1

#define SCTLR_EL1_EnDB                                  GENMASK(13, 13)
#define SCTLR_EL1_EnDB_MASK                             GENMASK(13, 13)
#define SCTLR_EL1_EnDB_SHIFT                            13
#define SCTLR_EL1_EnDB_WIDTH                            1

#define SCTLR_EL1_I                                     GENMASK(12, 12)
#define SCTLR_EL1_I_MASK                                GENMASK(12, 12)
#define SCTLR_EL1_I_SHIFT                               12
#define SCTLR_EL1_I_WIDTH                               1

#define SCTLR_EL1_EOS                                   GENMASK(11, 11)
#define SCTLR_EL1_EOS_MASK                              GENMASK(11, 11)
#define SCTLR_EL1_EOS_SHIFT                             11
#define SCTLR_EL1_EOS_WIDTH                             1

#define SCTLR_EL1_EnRCTX                                GENMASK(10, 10)
#define SCTLR_EL1_EnRCTX_MASK                           GENMASK(10, 10)
#define SCTLR_EL1_EnRCTX_SHIFT                          10
#define SCTLR_EL1_EnRCTX_WIDTH                          1

#define SCTLR_EL1_UMA                                   GENMASK(9, 9)
#define SCTLR_EL1_UMA_MASK                              GENMASK(9, 9)
#define SCTLR_EL1_UMA_SHIFT                             9
#define SCTLR_EL1_UMA_WIDTH                             1

#define SCTLR_EL1_SED                                   GENMASK(8, 8)
#define SCTLR_EL1_SED_MASK                              GENMASK(8, 8)
#define SCTLR_EL1_SED_SHIFT                             8
#define SCTLR_EL1_SED_WIDTH                             1

#define SCTLR_EL1_ITD                                   GENMASK(7, 7)
#define SCTLR_EL1_ITD_MASK                              GENMASK(7, 7)
#define SCTLR_EL1_ITD_SHIFT                             7
#define SCTLR_EL1_ITD_WIDTH                             1

#define SCTLR_EL1_nAA                                   GENMASK(6, 6)
#define SCTLR_EL1_nAA_MASK                              GENMASK(6, 6)
#define SCTLR_EL1_nAA_SHIFT                             6
#define SCTLR_EL1_nAA_WIDTH                             1

#define SCTLR_EL1_CP15BEN                               GENMASK(5, 5)
#define SCTLR_EL1_CP15BEN_MASK                          GENMASK(5, 5)
#define SCTLR_EL1_CP15BEN_SHIFT                         5
#define SCTLR_EL1_CP15BEN_WIDTH                         1

#define SCTLR_EL1_SA0                                   GENMASK(4, 4)
#define SCTLR_EL1_SA0_MASK                              GENMASK(4, 4)
#define SCTLR_EL1_SA0_SHIFT                             4
#define SCTLR_EL1_SA0_WIDTH                             1

#define SCTLR_EL1_SA                                    GENMASK(3, 3)
#define SCTLR_EL1_SA_MASK                               GENMASK(3, 3)
#define SCTLR_EL1_SA_SHIFT                              3
#define SCTLR_EL1_SA_WIDTH                              1

#define SCTLR_EL1_C                                     GENMASK(2, 2)
#define SCTLR_EL1_C_MASK                                GENMASK(2, 2)
#define SCTLR_EL1_C_SHIFT                               2
#define SCTLR_EL1_C_WIDTH                               1

#define SCTLR_EL1_A                                     GENMASK(1, 1)
#define SCTLR_EL1_A_MASK                                GENMASK(1, 1)
#define SCTLR_EL1_A_SHIFT                               1
#define SCTLR_EL1_A_WIDTH                               1

#define SCTLR_EL1_M                                     GENMASK(0, 0)
#define SCTLR_EL1_M_MASK                                GENMASK(0, 0)
#define SCTLR_EL1_M_SHIFT                               0
#define SCTLR_EL1_M_WIDTH                               1

#define SCTLR_EL1_RES0                                  (UL(0) | GENMASK_ULL(59, 58) | GENMASK_ULL(34, 34) | GENMASK_ULL(17, 17))
#define SCTLR_EL1_RES1                                  (UL(0))
#define SCTLR_EL1_UNKN                                  (UL(0))

#define CPACR_ELx_TTA                                   GENMASK(28, 28)
#define CPACR_ELx_TTA_MASK                              GENMASK(28, 28)
#define CPACR_ELx_TTA_SHIFT                             28
#define CPACR_ELx_TTA_WIDTH                             1

#define CPACR_ELx_SMEN                                  GENMASK(25, 24)
#define CPACR_ELx_SMEN_MASK                             GENMASK(25, 24)
#define CPACR_ELx_SMEN_SHIFT                            24
#define CPACR_ELx_SMEN_WIDTH                            2

#define CPACR_ELx_FPEN                                  GENMASK(21, 20)
#define CPACR_ELx_FPEN_MASK                             GENMASK(21, 20)
#define CPACR_ELx_FPEN_SHIFT                            20
#define CPACR_ELx_FPEN_WIDTH                            2

#define CPACR_ELx_ZEN                                   GENMASK(17, 16)
#define CPACR_ELx_ZEN_MASK                              GENMASK(17, 16)
#define CPACR_ELx_ZEN_SHIFT                             16
#define CPACR_ELx_ZEN_WIDTH                             2

#define CPACR_ELx_RES0                                  (UL(0) | GENMASK_ULL(63, 29) | GENMASK_ULL(27, 26) | GENMASK_ULL(23, 22) | GENMASK_ULL(19, 18) | GENMASK_ULL(15, 0))
#define CPACR_ELx_RES1                                  (UL(0))
#define CPACR_ELx_UNKN                                  (UL(0))

#define REG_CPACR_EL1                                   S3_0_C1_C0_2
#define SYS_CPACR_EL1                                   sys_reg(3, 0, 1, 0, 2)
#define SYS_CPACR_EL1_Op0                               3
#define SYS_CPACR_EL1_Op1                               0
#define SYS_CPACR_EL1_CRn                               1
#define SYS_CPACR_EL1_CRm                               0
#define SYS_CPACR_EL1_Op2                               2

/* For CPACR_EL1 fields see CPACR_ELx */

#define REG_SMPRI_EL1                                   S3_0_C1_C2_4
#define SYS_SMPRI_EL1                                   sys_reg(3, 0, 1, 2, 4)
#define SYS_SMPRI_EL1_Op0                               3
#define SYS_SMPRI_EL1_Op1                               0
#define SYS_SMPRI_EL1_CRn                               1
#define SYS_SMPRI_EL1_CRm                               2
#define SYS_SMPRI_EL1_Op2                               4

#define SMPRI_EL1_PRIORITY                              GENMASK(3, 0)
#define SMPRI_EL1_PRIORITY_MASK                         GENMASK(3, 0)
#define SMPRI_EL1_PRIORITY_SHIFT                        0
#define SMPRI_EL1_PRIORITY_WIDTH                        4

#define SMPRI_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 4))
#define SMPRI_EL1_RES1                                  (UL(0))
#define SMPRI_EL1_UNKN                                  (UL(0))

#define ZCR_ELx_LEN                                     GENMASK(3, 0)
#define ZCR_ELx_LEN_MASK                                GENMASK(3, 0)
#define ZCR_ELx_LEN_SHIFT                               0
#define ZCR_ELx_LEN_WIDTH                               4

#define ZCR_ELx_RES0                                    (UL(0) | GENMASK_ULL(63, 9))
#define ZCR_ELx_RES1                                    (UL(0))
#define ZCR_ELx_UNKN                                    (UL(0))

#define REG_ZCR_EL1                                     S3_0_C1_C2_0
#define SYS_ZCR_EL1                                     sys_reg(3, 0, 1, 2, 0)
#define SYS_ZCR_EL1_Op0                                 3
#define SYS_ZCR_EL1_Op1                                 0
#define SYS_ZCR_EL1_CRn                                 1
#define SYS_ZCR_EL1_CRm                                 2
#define SYS_ZCR_EL1_Op2                                 0

/* For ZCR_EL1 fields see ZCR_ELx */

#define SMCR_ELx_FA64                                   GENMASK(31, 31)
#define SMCR_ELx_FA64_MASK                              GENMASK(31, 31)
#define SMCR_ELx_FA64_SHIFT                             31
#define SMCR_ELx_FA64_WIDTH                             1

#define SMCR_ELx_EZT0                                   GENMASK(30, 30)
#define SMCR_ELx_EZT0_MASK                              GENMASK(30, 30)
#define SMCR_ELx_EZT0_SHIFT                             30
#define SMCR_ELx_EZT0_WIDTH                             1

#define SMCR_ELx_LEN                                    GENMASK(3, 0)
#define SMCR_ELx_LEN_MASK                               GENMASK(3, 0)
#define SMCR_ELx_LEN_SHIFT                              0
#define SMCR_ELx_LEN_WIDTH                              4

#define SMCR_ELx_RES0                                   (UL(0) | GENMASK_ULL(63, 32) | GENMASK_ULL(29, 9))
#define SMCR_ELx_RES1                                   (UL(0))
#define SMCR_ELx_UNKN                                   (UL(0))

#define REG_SMCR_EL1                                    S3_0_C1_C2_6
#define SYS_SMCR_EL1                                    sys_reg(3, 0, 1, 2, 6)
#define SYS_SMCR_EL1_Op0                                3
#define SYS_SMCR_EL1_Op1                                0
#define SYS_SMCR_EL1_CRn                                1
#define SYS_SMCR_EL1_CRm                                2
#define SYS_SMCR_EL1_Op2                                6

/* For SMCR_EL1 fields see SMCR_ELx */

#define REG_ALLINT                                      S3_0_C4_C3_0
#define SYS_ALLINT                                      sys_reg(3, 0, 4, 3, 0)
#define SYS_ALLINT_Op0                                  3
#define SYS_ALLINT_Op1                                  0
#define SYS_ALLINT_CRn                                  4
#define SYS_ALLINT_CRm                                  3
#define SYS_ALLINT_Op2                                  0

#define ALLINT_ALLINT                                   GENMASK(13, 13)
#define ALLINT_ALLINT_MASK                              GENMASK(13, 13)
#define ALLINT_ALLINT_SHIFT                             13
#define ALLINT_ALLINT_WIDTH                             1

#define ALLINT_RES0                                     (UL(0) | GENMASK_ULL(63, 14) | GENMASK_ULL(12, 0))
#define ALLINT_RES1                                     (UL(0))
#define ALLINT_UNKN                                     (UL(0))

#define REG_FAR_EL1                                     S3_0_C6_C0_0
#define SYS_FAR_EL1                                     sys_reg(3, 0, 6, 0, 0)
#define SYS_FAR_EL1_Op0                                 3
#define SYS_FAR_EL1_Op1                                 0
#define SYS_FAR_EL1_CRn                                 6
#define SYS_FAR_EL1_CRm                                 0
#define SYS_FAR_EL1_Op2                                 0

#define FAR_EL1_ADDR                                    GENMASK(63, 0)
#define FAR_EL1_ADDR_MASK                               GENMASK(63, 0)
#define FAR_EL1_ADDR_SHIFT                              0
#define FAR_EL1_ADDR_WIDTH                              64

#define FAR_EL1_RES0                                    (UL(0))
#define FAR_EL1_RES1                                    (UL(0))
#define FAR_EL1_UNKN                                    (UL(0))

#define REG_PMSCR_EL1                                   S3_0_C9_C9_0
#define SYS_PMSCR_EL1                                   sys_reg(3, 0, 9, 9, 0)
#define SYS_PMSCR_EL1_Op0                               3
#define SYS_PMSCR_EL1_Op1                               0
#define SYS_PMSCR_EL1_CRn                               9
#define SYS_PMSCR_EL1_CRm                               9
#define SYS_PMSCR_EL1_Op2                               0

#define PMSCR_EL1_PCT                                   GENMASK(7, 6)
#define PMSCR_EL1_PCT_MASK                              GENMASK(7, 6)
#define PMSCR_EL1_PCT_SHIFT                             6
#define PMSCR_EL1_PCT_WIDTH                             2

#define PMSCR_EL1_TS                                    GENMASK(5, 5)
#define PMSCR_EL1_TS_MASK                               GENMASK(5, 5)
#define PMSCR_EL1_TS_SHIFT                              5
#define PMSCR_EL1_TS_WIDTH                              1

#define PMSCR_EL1_PA                                    GENMASK(4, 4)
#define PMSCR_EL1_PA_MASK                               GENMASK(4, 4)
#define PMSCR_EL1_PA_SHIFT                              4
#define PMSCR_EL1_PA_WIDTH                              1

#define PMSCR_EL1_CX                                    GENMASK(3, 3)
#define PMSCR_EL1_CX_MASK                               GENMASK(3, 3)
#define PMSCR_EL1_CX_SHIFT                              3
#define PMSCR_EL1_CX_WIDTH                              1

#define PMSCR_EL1_E1SPE                                 GENMASK(1, 1)
#define PMSCR_EL1_E1SPE_MASK                            GENMASK(1, 1)
#define PMSCR_EL1_E1SPE_SHIFT                           1
#define PMSCR_EL1_E1SPE_WIDTH                           1

#define PMSCR_EL1_E0SPE                                 GENMASK(0, 0)
#define PMSCR_EL1_E0SPE_MASK                            GENMASK(0, 0)
#define PMSCR_EL1_E0SPE_SHIFT                           0
#define PMSCR_EL1_E0SPE_WIDTH                           1

#define PMSCR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 8) | GENMASK_ULL(2, 2))
#define PMSCR_EL1_RES1                                  (UL(0))
#define PMSCR_EL1_UNKN                                  (UL(0))

#define REG_PMSNEVFR_EL1                                S3_0_C9_C9_1
#define SYS_PMSNEVFR_EL1                                sys_reg(3, 0, 9, 9, 1)
#define SYS_PMSNEVFR_EL1_Op0                            3
#define SYS_PMSNEVFR_EL1_Op1                            0
#define SYS_PMSNEVFR_EL1_CRn                            9
#define SYS_PMSNEVFR_EL1_CRm                            9
#define SYS_PMSNEVFR_EL1_Op2                            1

#define PMSNEVFR_EL1_E                                  GENMASK(63, 0)
#define PMSNEVFR_EL1_E_MASK                             GENMASK(63, 0)
#define PMSNEVFR_EL1_E_SHIFT                            0
#define PMSNEVFR_EL1_E_WIDTH                            64

#define PMSNEVFR_EL1_RES0                               (UL(0))
#define PMSNEVFR_EL1_RES1                               (UL(0))
#define PMSNEVFR_EL1_UNKN                               (UL(0))

#define REG_PMSICR_EL1                                  S3_0_C9_C9_2
#define SYS_PMSICR_EL1                                  sys_reg(3, 0, 9, 9, 2)
#define SYS_PMSICR_EL1_Op0                              3
#define SYS_PMSICR_EL1_Op1                              0
#define SYS_PMSICR_EL1_CRn                              9
#define SYS_PMSICR_EL1_CRm                              9
#define SYS_PMSICR_EL1_Op2                              2

#define PMSICR_EL1_ECOUNT                               GENMASK(63, 56)
#define PMSICR_EL1_ECOUNT_MASK                          GENMASK(63, 56)
#define PMSICR_EL1_ECOUNT_SHIFT                         56
#define PMSICR_EL1_ECOUNT_WIDTH                         8

#define PMSICR_EL1_COUNT                                GENMASK(31, 0)
#define PMSICR_EL1_COUNT_MASK                           GENMASK(31, 0)
#define PMSICR_EL1_COUNT_SHIFT                          0
#define PMSICR_EL1_COUNT_WIDTH                          32

#define PMSICR_EL1_RES0                                 (UL(0) | GENMASK_ULL(55, 32))
#define PMSICR_EL1_RES1                                 (UL(0))
#define PMSICR_EL1_UNKN                                 (UL(0))

#define REG_PMSIRR_EL1                                  S3_0_C9_C9_3
#define SYS_PMSIRR_EL1                                  sys_reg(3, 0, 9, 9, 3)
#define SYS_PMSIRR_EL1_Op0                              3
#define SYS_PMSIRR_EL1_Op1                              0
#define SYS_PMSIRR_EL1_CRn                              9
#define SYS_PMSIRR_EL1_CRm                              9
#define SYS_PMSIRR_EL1_Op2                              3

#define PMSIRR_EL1_INTERVAL                             GENMASK(31, 8)
#define PMSIRR_EL1_INTERVAL_MASK                        GENMASK(31, 8)
#define PMSIRR_EL1_INTERVAL_SHIFT                       8
#define PMSIRR_EL1_INTERVAL_WIDTH                       24

#define PMSIRR_EL1_RND                                  GENMASK(0, 0)
#define PMSIRR_EL1_RND_MASK                             GENMASK(0, 0)
#define PMSIRR_EL1_RND_SHIFT                            0
#define PMSIRR_EL1_RND_WIDTH                            1

#define PMSIRR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 32) | GENMASK_ULL(7, 1))
#define PMSIRR_EL1_RES1                                 (UL(0))
#define PMSIRR_EL1_UNKN                                 (UL(0))

#define REG_PMSFCR_EL1                                  S3_0_C9_C9_4
#define SYS_PMSFCR_EL1                                  sys_reg(3, 0, 9, 9, 4)
#define SYS_PMSFCR_EL1_Op0                              3
#define SYS_PMSFCR_EL1_Op1                              0
#define SYS_PMSFCR_EL1_CRn                              9
#define SYS_PMSFCR_EL1_CRm                              9
#define SYS_PMSFCR_EL1_Op2                              4

#define PMSFCR_EL1_ST                                   GENMASK(18, 18)
#define PMSFCR_EL1_ST_MASK                              GENMASK(18, 18)
#define PMSFCR_EL1_ST_SHIFT                             18
#define PMSFCR_EL1_ST_WIDTH                             1

#define PMSFCR_EL1_LD                                   GENMASK(17, 17)
#define PMSFCR_EL1_LD_MASK                              GENMASK(17, 17)
#define PMSFCR_EL1_LD_SHIFT                             17
#define PMSFCR_EL1_LD_WIDTH                             1

#define PMSFCR_EL1_B                                    GENMASK(16, 16)
#define PMSFCR_EL1_B_MASK                               GENMASK(16, 16)
#define PMSFCR_EL1_B_SHIFT                              16
#define PMSFCR_EL1_B_WIDTH                              1

#define PMSFCR_EL1_FnE                                  GENMASK(3, 3)
#define PMSFCR_EL1_FnE_MASK                             GENMASK(3, 3)
#define PMSFCR_EL1_FnE_SHIFT                            3
#define PMSFCR_EL1_FnE_WIDTH                            1

#define PMSFCR_EL1_FL                                   GENMASK(2, 2)
#define PMSFCR_EL1_FL_MASK                              GENMASK(2, 2)
#define PMSFCR_EL1_FL_SHIFT                             2
#define PMSFCR_EL1_FL_WIDTH                             1

#define PMSFCR_EL1_FT                                   GENMASK(1, 1)
#define PMSFCR_EL1_FT_MASK                              GENMASK(1, 1)
#define PMSFCR_EL1_FT_SHIFT                             1
#define PMSFCR_EL1_FT_WIDTH                             1

#define PMSFCR_EL1_FE                                   GENMASK(0, 0)
#define PMSFCR_EL1_FE_MASK                              GENMASK(0, 0)
#define PMSFCR_EL1_FE_SHIFT                             0
#define PMSFCR_EL1_FE_WIDTH                             1

#define PMSFCR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 19) | GENMASK_ULL(15, 4))
#define PMSFCR_EL1_RES1                                 (UL(0))
#define PMSFCR_EL1_UNKN                                 (UL(0))

#define REG_PMSEVFR_EL1                                 S3_0_C9_C9_5
#define SYS_PMSEVFR_EL1                                 sys_reg(3, 0, 9, 9, 5)
#define SYS_PMSEVFR_EL1_Op0                             3
#define SYS_PMSEVFR_EL1_Op1                             0
#define SYS_PMSEVFR_EL1_CRn                             9
#define SYS_PMSEVFR_EL1_CRm                             9
#define SYS_PMSEVFR_EL1_Op2                             5

#define PMSEVFR_EL1_E                                   GENMASK(63, 0)
#define PMSEVFR_EL1_E_MASK                              GENMASK(63, 0)
#define PMSEVFR_EL1_E_SHIFT                             0
#define PMSEVFR_EL1_E_WIDTH                             64

#define PMSEVFR_EL1_RES0                                (UL(0))
#define PMSEVFR_EL1_RES1                                (UL(0))
#define PMSEVFR_EL1_UNKN                                (UL(0))

#define REG_PMSLATFR_EL1                                S3_0_C9_C9_6
#define SYS_PMSLATFR_EL1                                sys_reg(3, 0, 9, 9, 6)
#define SYS_PMSLATFR_EL1_Op0                            3
#define SYS_PMSLATFR_EL1_Op1                            0
#define SYS_PMSLATFR_EL1_CRn                            9
#define SYS_PMSLATFR_EL1_CRm                            9
#define SYS_PMSLATFR_EL1_Op2                            6

#define PMSLATFR_EL1_MINLAT                             GENMASK(15, 0)
#define PMSLATFR_EL1_MINLAT_MASK                        GENMASK(15, 0)
#define PMSLATFR_EL1_MINLAT_SHIFT                       0
#define PMSLATFR_EL1_MINLAT_WIDTH                       16

#define PMSLATFR_EL1_RES0                               (UL(0) | GENMASK_ULL(63, 16))
#define PMSLATFR_EL1_RES1                               (UL(0))
#define PMSLATFR_EL1_UNKN                               (UL(0))

#define REG_PMSIDR_EL1                                  S3_0_C9_C9_7
#define SYS_PMSIDR_EL1                                  sys_reg(3, 0, 9, 9, 7)
#define SYS_PMSIDR_EL1_Op0                              3
#define SYS_PMSIDR_EL1_Op1                              0
#define SYS_PMSIDR_EL1_CRn                              9
#define SYS_PMSIDR_EL1_CRm                              9
#define SYS_PMSIDR_EL1_Op2                              7

#define PMSIDR_EL1_PBT                                  GENMASK(24, 24)
#define PMSIDR_EL1_PBT_MASK                             GENMASK(24, 24)
#define PMSIDR_EL1_PBT_SHIFT                            24
#define PMSIDR_EL1_PBT_WIDTH                            1

#define PMSIDR_EL1_FORMAT                               GENMASK(23, 20)
#define PMSIDR_EL1_FORMAT_MASK                          GENMASK(23, 20)
#define PMSIDR_EL1_FORMAT_SHIFT                         20
#define PMSIDR_EL1_FORMAT_WIDTH                         4

#define PMSIDR_EL1_COUNTSIZE                            GENMASK(19, 16)
#define PMSIDR_EL1_COUNTSIZE_MASK                       GENMASK(19, 16)
#define PMSIDR_EL1_COUNTSIZE_SHIFT                      16
#define PMSIDR_EL1_COUNTSIZE_WIDTH                      4
#define PMSIDR_EL1_COUNTSIZE_12_BIT_SAT                 UL(0b0010)
#define PMSIDR_EL1_COUNTSIZE_16_BIT_SAT                 UL(0b0011)

#define PMSIDR_EL1_MAXSIZE                              GENMASK(15, 12)
#define PMSIDR_EL1_MAXSIZE_MASK                         GENMASK(15, 12)
#define PMSIDR_EL1_MAXSIZE_SHIFT                        12
#define PMSIDR_EL1_MAXSIZE_WIDTH                        4

#define PMSIDR_EL1_INTERVAL                             GENMASK(11, 8)
#define PMSIDR_EL1_INTERVAL_MASK                        GENMASK(11, 8)
#define PMSIDR_EL1_INTERVAL_SHIFT                       8
#define PMSIDR_EL1_INTERVAL_WIDTH                       4
#define PMSIDR_EL1_INTERVAL_256                         UL(0b0000)
#define PMSIDR_EL1_INTERVAL_512                         UL(0b0010)
#define PMSIDR_EL1_INTERVAL_768                         UL(0b0011)
#define PMSIDR_EL1_INTERVAL_1024                        UL(0b0100)
#define PMSIDR_EL1_INTERVAL_1536                        UL(0b0101)
#define PMSIDR_EL1_INTERVAL_2048                        UL(0b0110)
#define PMSIDR_EL1_INTERVAL_3072                        UL(0b0111)
#define PMSIDR_EL1_INTERVAL_4096                        UL(0b1000)

#define PMSIDR_EL1_FnE                                  GENMASK(6, 6)
#define PMSIDR_EL1_FnE_MASK                             GENMASK(6, 6)
#define PMSIDR_EL1_FnE_SHIFT                            6
#define PMSIDR_EL1_FnE_WIDTH                            1

#define PMSIDR_EL1_ERND                                 GENMASK(5, 5)
#define PMSIDR_EL1_ERND_MASK                            GENMASK(5, 5)
#define PMSIDR_EL1_ERND_SHIFT                           5
#define PMSIDR_EL1_ERND_WIDTH                           1

#define PMSIDR_EL1_LDS                                  GENMASK(4, 4)
#define PMSIDR_EL1_LDS_MASK                             GENMASK(4, 4)
#define PMSIDR_EL1_LDS_SHIFT                            4
#define PMSIDR_EL1_LDS_WIDTH                            1

#define PMSIDR_EL1_ARCHINST                             GENMASK(3, 3)
#define PMSIDR_EL1_ARCHINST_MASK                        GENMASK(3, 3)
#define PMSIDR_EL1_ARCHINST_SHIFT                       3
#define PMSIDR_EL1_ARCHINST_WIDTH                       1

#define PMSIDR_EL1_FL                                   GENMASK(2, 2)
#define PMSIDR_EL1_FL_MASK                              GENMASK(2, 2)
#define PMSIDR_EL1_FL_SHIFT                             2
#define PMSIDR_EL1_FL_WIDTH                             1

#define PMSIDR_EL1_FT                                   GENMASK(1, 1)
#define PMSIDR_EL1_FT_MASK                              GENMASK(1, 1)
#define PMSIDR_EL1_FT_SHIFT                             1
#define PMSIDR_EL1_FT_WIDTH                             1

#define PMSIDR_EL1_FE                                   GENMASK(0, 0)
#define PMSIDR_EL1_FE_MASK                              GENMASK(0, 0)
#define PMSIDR_EL1_FE_SHIFT                             0
#define PMSIDR_EL1_FE_WIDTH                             1

#define PMSIDR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 25) | GENMASK_ULL(7, 7))
#define PMSIDR_EL1_RES1                                 (UL(0))
#define PMSIDR_EL1_UNKN                                 (UL(0))

#define REG_PMBLIMITR_EL1                               S3_0_C9_C10_0
#define SYS_PMBLIMITR_EL1                               sys_reg(3, 0, 9, 10, 0)
#define SYS_PMBLIMITR_EL1_Op0                           3
#define SYS_PMBLIMITR_EL1_Op1                           0
#define SYS_PMBLIMITR_EL1_CRn                           9
#define SYS_PMBLIMITR_EL1_CRm                           10
#define SYS_PMBLIMITR_EL1_Op2                           0

#define PMBLIMITR_EL1_LIMIT                             GENMASK(63, 12)
#define PMBLIMITR_EL1_LIMIT_MASK                        GENMASK(63, 12)
#define PMBLIMITR_EL1_LIMIT_SHIFT                       12
#define PMBLIMITR_EL1_LIMIT_WIDTH                       52

#define PMBLIMITR_EL1_PMFZ                              GENMASK(5, 5)
#define PMBLIMITR_EL1_PMFZ_MASK                         GENMASK(5, 5)
#define PMBLIMITR_EL1_PMFZ_SHIFT                        5
#define PMBLIMITR_EL1_PMFZ_WIDTH                        1

#define PMBLIMITR_EL1_FM                                GENMASK(2, 1)
#define PMBLIMITR_EL1_FM_MASK                           GENMASK(2, 1)
#define PMBLIMITR_EL1_FM_SHIFT                          1
#define PMBLIMITR_EL1_FM_WIDTH                          2
#define PMBLIMITR_EL1_FM_FILL                           UL(0b00)
#define PMBLIMITR_EL1_FM_DISCARD                        UL(0b10)

#define PMBLIMITR_EL1_E                                 GENMASK(0, 0)
#define PMBLIMITR_EL1_E_MASK                            GENMASK(0, 0)
#define PMBLIMITR_EL1_E_SHIFT                           0
#define PMBLIMITR_EL1_E_WIDTH                           1

#define PMBLIMITR_EL1_RES0                              (UL(0) | GENMASK_ULL(11, 6) | GENMASK_ULL(4, 3))
#define PMBLIMITR_EL1_RES1                              (UL(0))
#define PMBLIMITR_EL1_UNKN                              (UL(0))

#define REG_PMBPTR_EL1                                  S3_0_C9_C10_1
#define SYS_PMBPTR_EL1                                  sys_reg(3, 0, 9, 10, 1)
#define SYS_PMBPTR_EL1_Op0                              3
#define SYS_PMBPTR_EL1_Op1                              0
#define SYS_PMBPTR_EL1_CRn                              9
#define SYS_PMBPTR_EL1_CRm                              10
#define SYS_PMBPTR_EL1_Op2                              1

#define PMBPTR_EL1_PTR                                  GENMASK(63, 0)
#define PMBPTR_EL1_PTR_MASK                             GENMASK(63, 0)
#define PMBPTR_EL1_PTR_SHIFT                            0
#define PMBPTR_EL1_PTR_WIDTH                            64

#define PMBPTR_EL1_RES0                                 (UL(0))
#define PMBPTR_EL1_RES1                                 (UL(0))
#define PMBPTR_EL1_UNKN                                 (UL(0))

#define REG_PMBSR_EL1                                   S3_0_C9_C10_3
#define SYS_PMBSR_EL1                                   sys_reg(3, 0, 9, 10, 3)
#define SYS_PMBSR_EL1_Op0                               3
#define SYS_PMBSR_EL1_Op1                               0
#define SYS_PMBSR_EL1_CRn                               9
#define SYS_PMBSR_EL1_CRm                               10
#define SYS_PMBSR_EL1_Op2                               3

#define PMBSR_EL1_EC                                    GENMASK(31, 26)
#define PMBSR_EL1_EC_MASK                               GENMASK(31, 26)
#define PMBSR_EL1_EC_SHIFT                              26
#define PMBSR_EL1_EC_WIDTH                              6
#define PMBSR_EL1_EC_BUF                                UL(0b000000)
#define PMBSR_EL1_EC_FAULT_S1                           UL(0b100100)
#define PMBSR_EL1_EC_FAULT_S2                           UL(0b100101)
#define PMBSR_EL1_EC_FAULT_GPC                          UL(0b011110)
#define PMBSR_EL1_EC_IMP_DEF                            UL(0b011111)

#define PMBSR_EL1_DL                                    GENMASK(19, 19)
#define PMBSR_EL1_DL_MASK                               GENMASK(19, 19)
#define PMBSR_EL1_DL_SHIFT                              19
#define PMBSR_EL1_DL_WIDTH                              1

#define PMBSR_EL1_EA                                    GENMASK(18, 18)
#define PMBSR_EL1_EA_MASK                               GENMASK(18, 18)
#define PMBSR_EL1_EA_SHIFT                              18
#define PMBSR_EL1_EA_WIDTH                              1

#define PMBSR_EL1_S                                     GENMASK(17, 17)
#define PMBSR_EL1_S_MASK                                GENMASK(17, 17)
#define PMBSR_EL1_S_SHIFT                               17
#define PMBSR_EL1_S_WIDTH                               1

#define PMBSR_EL1_COLL                                  GENMASK(16, 16)
#define PMBSR_EL1_COLL_MASK                             GENMASK(16, 16)
#define PMBSR_EL1_COLL_SHIFT                            16
#define PMBSR_EL1_COLL_WIDTH                            1

#define PMBSR_EL1_MSS                                   GENMASK(15, 0)
#define PMBSR_EL1_MSS_MASK                              GENMASK(15, 0)
#define PMBSR_EL1_MSS_SHIFT                             0
#define PMBSR_EL1_MSS_WIDTH                             16

#define PMBSR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 32) | GENMASK_ULL(25, 20))
#define PMBSR_EL1_RES1                                  (UL(0))
#define PMBSR_EL1_UNKN                                  (UL(0))

#define REG_PMBIDR_EL1                                  S3_0_C9_C10_7
#define SYS_PMBIDR_EL1                                  sys_reg(3, 0, 9, 10, 7)
#define SYS_PMBIDR_EL1_Op0                              3
#define SYS_PMBIDR_EL1_Op1                              0
#define SYS_PMBIDR_EL1_CRn                              9
#define SYS_PMBIDR_EL1_CRm                              10
#define SYS_PMBIDR_EL1_Op2                              7

#define PMBIDR_EL1_EA                                   GENMASK(11, 8)
#define PMBIDR_EL1_EA_MASK                              GENMASK(11, 8)
#define PMBIDR_EL1_EA_SHIFT                             8
#define PMBIDR_EL1_EA_WIDTH                             4
#define PMBIDR_EL1_EA_NotDescribed                      UL(0b0000)
#define PMBIDR_EL1_EA_Ignored                           UL(0b0001)
#define PMBIDR_EL1_EA_SError                            UL(0b0010)

#define PMBIDR_EL1_F                                    GENMASK(5, 5)
#define PMBIDR_EL1_F_MASK                               GENMASK(5, 5)
#define PMBIDR_EL1_F_SHIFT                              5
#define PMBIDR_EL1_F_WIDTH                              1

#define PMBIDR_EL1_P                                    GENMASK(4, 4)
#define PMBIDR_EL1_P_MASK                               GENMASK(4, 4)
#define PMBIDR_EL1_P_SHIFT                              4
#define PMBIDR_EL1_P_WIDTH                              1

#define PMBIDR_EL1_ALIGN                                GENMASK(3, 0)
#define PMBIDR_EL1_ALIGN_MASK                           GENMASK(3, 0)
#define PMBIDR_EL1_ALIGN_SHIFT                          0
#define PMBIDR_EL1_ALIGN_WIDTH                          4

#define PMBIDR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 12) | GENMASK_ULL(7, 6))
#define PMBIDR_EL1_RES1                                 (UL(0))
#define PMBIDR_EL1_UNKN                                 (UL(0))

#define CONTEXTIDR_ELx_PROCID                           GENMASK(31, 0)
#define CONTEXTIDR_ELx_PROCID_MASK                      GENMASK(31, 0)
#define CONTEXTIDR_ELx_PROCID_SHIFT                     0
#define CONTEXTIDR_ELx_PROCID_WIDTH                     32

#define CONTEXTIDR_ELx_RES0                             (UL(0) | GENMASK_ULL(63, 32))
#define CONTEXTIDR_ELx_RES1                             (UL(0))
#define CONTEXTIDR_ELx_UNKN                             (UL(0))

#define REG_CONTEXTIDR_EL1                              S3_0_C13_C0_1
#define SYS_CONTEXTIDR_EL1                              sys_reg(3, 0, 13, 0, 1)
#define SYS_CONTEXTIDR_EL1_Op0                          3
#define SYS_CONTEXTIDR_EL1_Op1                          0
#define SYS_CONTEXTIDR_EL1_CRn                          13
#define SYS_CONTEXTIDR_EL1_CRm                          0
#define SYS_CONTEXTIDR_EL1_Op2                          1

/* For CONTEXTIDR_EL1 fields see CONTEXTIDR_ELx */

#define REG_TPIDR_EL1                                   S3_0_C13_C0_4
#define SYS_TPIDR_EL1                                   sys_reg(3, 0, 13, 0, 4)
#define SYS_TPIDR_EL1_Op0                               3
#define SYS_TPIDR_EL1_Op1                               0
#define SYS_TPIDR_EL1_CRn                               13
#define SYS_TPIDR_EL1_CRm                               0
#define SYS_TPIDR_EL1_Op2                               4

#define TPIDR_EL1_ThreadID                              GENMASK(63, 0)
#define TPIDR_EL1_ThreadID_MASK                         GENMASK(63, 0)
#define TPIDR_EL1_ThreadID_SHIFT                        0
#define TPIDR_EL1_ThreadID_WIDTH                        64

#define TPIDR_EL1_RES0                                  (UL(0))
#define TPIDR_EL1_RES1                                  (UL(0))
#define TPIDR_EL1_UNKN                                  (UL(0))

#define REG_SCXTNUM_EL1                                 S3_0_C13_C0_7
#define SYS_SCXTNUM_EL1                                 sys_reg(3, 0, 13, 0, 7)
#define SYS_SCXTNUM_EL1_Op0                             3
#define SYS_SCXTNUM_EL1_Op1                             0
#define SYS_SCXTNUM_EL1_CRn                             13
#define SYS_SCXTNUM_EL1_CRm                             0
#define SYS_SCXTNUM_EL1_Op2                             7

#define SCXTNUM_EL1_SoftwareContextNumber               GENMASK(63, 0)
#define SCXTNUM_EL1_SoftwareContextNumber_MASK          GENMASK(63, 0)
#define SCXTNUM_EL1_SoftwareContextNumber_SHIFT         0
#define SCXTNUM_EL1_SoftwareContextNumber_WIDTH         64

#define SCXTNUM_EL1_RES0                                (UL(0))
#define SCXTNUM_EL1_RES1                                (UL(0))
#define SCXTNUM_EL1_UNKN                                (UL(0))

#define REG_CCSIDR_EL1                                  S3_1_C0_C0_0
#define SYS_CCSIDR_EL1                                  sys_reg(3, 1, 0, 0, 0)
#define SYS_CCSIDR_EL1_Op0                              3
#define SYS_CCSIDR_EL1_Op1                              1
#define SYS_CCSIDR_EL1_CRn                              0
#define SYS_CCSIDR_EL1_CRm                              0
#define SYS_CCSIDR_EL1_Op2                              0

#define CCSIDR_EL1_NumSets                              GENMASK(27, 13)
#define CCSIDR_EL1_NumSets_MASK                         GENMASK(27, 13)
#define CCSIDR_EL1_NumSets_SHIFT                        13
#define CCSIDR_EL1_NumSets_WIDTH                        15

#define CCSIDR_EL1_Associativity                        GENMASK(12, 3)
#define CCSIDR_EL1_Associativity_MASK                   GENMASK(12, 3)
#define CCSIDR_EL1_Associativity_SHIFT                  3
#define CCSIDR_EL1_Associativity_WIDTH                  10

#define CCSIDR_EL1_LineSize                             GENMASK(2, 0)
#define CCSIDR_EL1_LineSize_MASK                        GENMASK(2, 0)
#define CCSIDR_EL1_LineSize_SHIFT                       0
#define CCSIDR_EL1_LineSize_WIDTH                       3

#define CCSIDR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 32))
#define CCSIDR_EL1_RES1                                 (UL(0))
#define CCSIDR_EL1_UNKN                                 (UL(0) | GENMASK_ULL(31, 28))

#define REG_CLIDR_EL1                                   S3_1_C0_C0_1
#define SYS_CLIDR_EL1                                   sys_reg(3, 1, 0, 0, 1)
#define SYS_CLIDR_EL1_Op0                               3
#define SYS_CLIDR_EL1_Op1                               1
#define SYS_CLIDR_EL1_CRn                               0
#define SYS_CLIDR_EL1_CRm                               0
#define SYS_CLIDR_EL1_Op2                               1

#define CLIDR_EL1_Ttypen                                GENMASK(46, 33)
#define CLIDR_EL1_Ttypen_MASK                           GENMASK(46, 33)
#define CLIDR_EL1_Ttypen_SHIFT                          33
#define CLIDR_EL1_Ttypen_WIDTH                          14

#define CLIDR_EL1_ICB                                   GENMASK(32, 30)
#define CLIDR_EL1_ICB_MASK                              GENMASK(32, 30)
#define CLIDR_EL1_ICB_SHIFT                             30
#define CLIDR_EL1_ICB_WIDTH                             3

#define CLIDR_EL1_LoUU                                  GENMASK(29, 27)
#define CLIDR_EL1_LoUU_MASK                             GENMASK(29, 27)
#define CLIDR_EL1_LoUU_SHIFT                            27
#define CLIDR_EL1_LoUU_WIDTH                            3

#define CLIDR_EL1_LoC                                   GENMASK(26, 24)
#define CLIDR_EL1_LoC_MASK                              GENMASK(26, 24)
#define CLIDR_EL1_LoC_SHIFT                             24
#define CLIDR_EL1_LoC_WIDTH                             3

#define CLIDR_EL1_LoUIS                                 GENMASK(23, 21)
#define CLIDR_EL1_LoUIS_MASK                            GENMASK(23, 21)
#define CLIDR_EL1_LoUIS_SHIFT                           21
#define CLIDR_EL1_LoUIS_WIDTH                           3

#define CLIDR_EL1_Ctype7                                GENMASK(20, 18)
#define CLIDR_EL1_Ctype7_MASK                           GENMASK(20, 18)
#define CLIDR_EL1_Ctype7_SHIFT                          18
#define CLIDR_EL1_Ctype7_WIDTH                          3

#define CLIDR_EL1_Ctype6                                GENMASK(17, 15)
#define CLIDR_EL1_Ctype6_MASK                           GENMASK(17, 15)
#define CLIDR_EL1_Ctype6_SHIFT                          15
#define CLIDR_EL1_Ctype6_WIDTH                          3

#define CLIDR_EL1_Ctype5                                GENMASK(14, 12)
#define CLIDR_EL1_Ctype5_MASK                           GENMASK(14, 12)
#define CLIDR_EL1_Ctype5_SHIFT                          12
#define CLIDR_EL1_Ctype5_WIDTH                          3

#define CLIDR_EL1_Ctype4                                GENMASK(11, 9)
#define CLIDR_EL1_Ctype4_MASK                           GENMASK(11, 9)
#define CLIDR_EL1_Ctype4_SHIFT                          9
#define CLIDR_EL1_Ctype4_WIDTH                          3

#define CLIDR_EL1_Ctype3                                GENMASK(8, 6)
#define CLIDR_EL1_Ctype3_MASK                           GENMASK(8, 6)
#define CLIDR_EL1_Ctype3_SHIFT                          6
#define CLIDR_EL1_Ctype3_WIDTH                          3

#define CLIDR_EL1_Ctype2                                GENMASK(5, 3)
#define CLIDR_EL1_Ctype2_MASK                           GENMASK(5, 3)
#define CLIDR_EL1_Ctype2_SHIFT                          3
#define CLIDR_EL1_Ctype2_WIDTH                          3

#define CLIDR_EL1_Ctype1                                GENMASK(2, 0)
#define CLIDR_EL1_Ctype1_MASK                           GENMASK(2, 0)
#define CLIDR_EL1_Ctype1_SHIFT                          0
#define CLIDR_EL1_Ctype1_WIDTH                          3

#define CLIDR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 47))
#define CLIDR_EL1_RES1                                  (UL(0))
#define CLIDR_EL1_UNKN                                  (UL(0))

#define REG_CCSIDR2_EL1                                 S3_1_C0_C0_2
#define SYS_CCSIDR2_EL1                                 sys_reg(3, 1, 0, 0, 2)
#define SYS_CCSIDR2_EL1_Op0                             3
#define SYS_CCSIDR2_EL1_Op1                             1
#define SYS_CCSIDR2_EL1_CRn                             0
#define SYS_CCSIDR2_EL1_CRm                             0
#define SYS_CCSIDR2_EL1_Op2                             2

#define CCSIDR2_EL1_NumSets                             GENMASK(23, 0)
#define CCSIDR2_EL1_NumSets_MASK                        GENMASK(23, 0)
#define CCSIDR2_EL1_NumSets_SHIFT                       0
#define CCSIDR2_EL1_NumSets_WIDTH                       24

#define CCSIDR2_EL1_RES0                                (UL(0) | GENMASK_ULL(63, 24))
#define CCSIDR2_EL1_RES1                                (UL(0))
#define CCSIDR2_EL1_UNKN                                (UL(0))

#define REG_GMID_EL1                                    S3_1_C0_C0_4
#define SYS_GMID_EL1                                    sys_reg(3, 1, 0, 0, 4)
#define SYS_GMID_EL1_Op0                                3
#define SYS_GMID_EL1_Op1                                1
#define SYS_GMID_EL1_CRn                                0
#define SYS_GMID_EL1_CRm                                0
#define SYS_GMID_EL1_Op2                                4

#define GMID_EL1_BS                                     GENMASK(3, 0)
#define GMID_EL1_BS_MASK                                GENMASK(3, 0)
#define GMID_EL1_BS_SHIFT                               0
#define GMID_EL1_BS_WIDTH                               4

#define GMID_EL1_RES0                                   (UL(0) | GENMASK_ULL(63, 4))
#define GMID_EL1_RES1                                   (UL(0))
#define GMID_EL1_UNKN                                   (UL(0))

#define REG_SMIDR_EL1                                   S3_1_C0_C0_6
#define SYS_SMIDR_EL1                                   sys_reg(3, 1, 0, 0, 6)
#define SYS_SMIDR_EL1_Op0                               3
#define SYS_SMIDR_EL1_Op1                               1
#define SYS_SMIDR_EL1_CRn                               0
#define SYS_SMIDR_EL1_CRm                               0
#define SYS_SMIDR_EL1_Op2                               6

#define SMIDR_EL1_IMPLEMENTER                           GENMASK(31, 24)
#define SMIDR_EL1_IMPLEMENTER_MASK                      GENMASK(31, 24)
#define SMIDR_EL1_IMPLEMENTER_SHIFT                     24
#define SMIDR_EL1_IMPLEMENTER_WIDTH                     8

#define SMIDR_EL1_REVISION                              GENMASK(23, 16)
#define SMIDR_EL1_REVISION_MASK                         GENMASK(23, 16)
#define SMIDR_EL1_REVISION_SHIFT                        16
#define SMIDR_EL1_REVISION_WIDTH                        8

#define SMIDR_EL1_SMPS                                  GENMASK(15, 15)
#define SMIDR_EL1_SMPS_MASK                             GENMASK(15, 15)
#define SMIDR_EL1_SMPS_SHIFT                            15
#define SMIDR_EL1_SMPS_WIDTH                            1

#define SMIDR_EL1_AFFINITY                              GENMASK(11, 0)
#define SMIDR_EL1_AFFINITY_MASK                         GENMASK(11, 0)
#define SMIDR_EL1_AFFINITY_SHIFT                        0
#define SMIDR_EL1_AFFINITY_WIDTH                        12

#define SMIDR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 32) | GENMASK_ULL(14, 12))
#define SMIDR_EL1_RES1                                  (UL(0))
#define SMIDR_EL1_UNKN                                  (UL(0))

#define REG_CSSELR_EL1                                  S3_2_C0_C0_0
#define SYS_CSSELR_EL1                                  sys_reg(3, 2, 0, 0, 0)
#define SYS_CSSELR_EL1_Op0                              3
#define SYS_CSSELR_EL1_Op1                              2
#define SYS_CSSELR_EL1_CRn                              0
#define SYS_CSSELR_EL1_CRm                              0
#define SYS_CSSELR_EL1_Op2                              0

#define CSSELR_EL1_TnD                                  GENMASK(4, 4)
#define CSSELR_EL1_TnD_MASK                             GENMASK(4, 4)
#define CSSELR_EL1_TnD_SHIFT                            4
#define CSSELR_EL1_TnD_WIDTH                            1

#define CSSELR_EL1_Level                                GENMASK(3, 1)
#define CSSELR_EL1_Level_MASK                           GENMASK(3, 1)
#define CSSELR_EL1_Level_SHIFT                          1
#define CSSELR_EL1_Level_WIDTH                          3

#define CSSELR_EL1_InD                                  GENMASK(0, 0)
#define CSSELR_EL1_InD_MASK                             GENMASK(0, 0)
#define CSSELR_EL1_InD_SHIFT                            0
#define CSSELR_EL1_InD_WIDTH                            1

#define CSSELR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 5))
#define CSSELR_EL1_RES1                                 (UL(0))
#define CSSELR_EL1_UNKN                                 (UL(0))

#define REG_CTR_EL0                                     S3_3_C0_C0_1
#define SYS_CTR_EL0                                     sys_reg(3, 3, 0, 0, 1)
#define SYS_CTR_EL0_Op0                                 3
#define SYS_CTR_EL0_Op1                                 3
#define SYS_CTR_EL0_CRn                                 0
#define SYS_CTR_EL0_CRm                                 0
#define SYS_CTR_EL0_Op2                                 1

#define CTR_EL0_TminLine                                GENMASK(37, 32)
#define CTR_EL0_TminLine_MASK                           GENMASK(37, 32)
#define CTR_EL0_TminLine_SHIFT                          32
#define CTR_EL0_TminLine_WIDTH                          6

#define CTR_EL0_DIC                                     GENMASK(29, 29)
#define CTR_EL0_DIC_MASK                                GENMASK(29, 29)
#define CTR_EL0_DIC_SHIFT                               29
#define CTR_EL0_DIC_WIDTH                               1

#define CTR_EL0_IDC                                     GENMASK(28, 28)
#define CTR_EL0_IDC_MASK                                GENMASK(28, 28)
#define CTR_EL0_IDC_SHIFT                               28
#define CTR_EL0_IDC_WIDTH                               1

#define CTR_EL0_CWG                                     GENMASK(27, 24)
#define CTR_EL0_CWG_MASK                                GENMASK(27, 24)
#define CTR_EL0_CWG_SHIFT                               24
#define CTR_EL0_CWG_WIDTH                               4

#define CTR_EL0_ERG                                     GENMASK(23, 20)
#define CTR_EL0_ERG_MASK                                GENMASK(23, 20)
#define CTR_EL0_ERG_SHIFT                               20
#define CTR_EL0_ERG_WIDTH                               4

#define CTR_EL0_DminLine                                GENMASK(19, 16)
#define CTR_EL0_DminLine_MASK                           GENMASK(19, 16)
#define CTR_EL0_DminLine_SHIFT                          16
#define CTR_EL0_DminLine_WIDTH                          4

#define CTR_EL0_L1Ip                                    GENMASK(15, 14)
#define CTR_EL0_L1Ip_MASK                               GENMASK(15, 14)
#define CTR_EL0_L1Ip_SHIFT                              14
#define CTR_EL0_L1Ip_WIDTH                              2
#define CTR_EL0_L1Ip_VPIPT                              UL(0b00)
#define CTR_EL0_L1Ip_RESERVED                           UL(0b01)
#define CTR_EL0_L1Ip_VIPT                               UL(0b10)
#define CTR_EL0_L1Ip_PIPT                               UL(0b11)

#define CTR_EL0_IminLine                                GENMASK(3, 0)
#define CTR_EL0_IminLine_MASK                           GENMASK(3, 0)
#define CTR_EL0_IminLine_SHIFT                          0
#define CTR_EL0_IminLine_WIDTH                          4

#define CTR_EL0_RES0                                    (UL(0) | GENMASK_ULL(63, 38) | GENMASK_ULL(30, 30) | GENMASK_ULL(13, 4))
#define CTR_EL0_RES1                                    (UL(0) | GENMASK_ULL(31, 31))
#define CTR_EL0_UNKN                                    (UL(0))

#define REG_DCZID_EL0                                   S3_3_C0_C0_7
#define SYS_DCZID_EL0                                   sys_reg(3, 3, 0, 0, 7)
#define SYS_DCZID_EL0_Op0                               3
#define SYS_DCZID_EL0_Op1                               3
#define SYS_DCZID_EL0_CRn                               0
#define SYS_DCZID_EL0_CRm                               0
#define SYS_DCZID_EL0_Op2                               7

#define DCZID_EL0_DZP                                   GENMASK(4, 4)
#define DCZID_EL0_DZP_MASK                              GENMASK(4, 4)
#define DCZID_EL0_DZP_SHIFT                             4
#define DCZID_EL0_DZP_WIDTH                             1

#define DCZID_EL0_BS                                    GENMASK(3, 0)
#define DCZID_EL0_BS_MASK                               GENMASK(3, 0)
#define DCZID_EL0_BS_SHIFT                              0
#define DCZID_EL0_BS_WIDTH                              4

#define DCZID_EL0_RES0                                  (UL(0) | GENMASK_ULL(63, 5))
#define DCZID_EL0_RES1                                  (UL(0))
#define DCZID_EL0_UNKN                                  (UL(0))

#define REG_SVCR                                        S3_3_C4_C2_2
#define SYS_SVCR                                        sys_reg(3, 3, 4, 2, 2)
#define SYS_SVCR_Op0                                    3
#define SYS_SVCR_Op1                                    3
#define SYS_SVCR_CRn                                    4
#define SYS_SVCR_CRm                                    2
#define SYS_SVCR_Op2                                    2

#define SVCR_ZA                                         GENMASK(1, 1)
#define SVCR_ZA_MASK                                    GENMASK(1, 1)
#define SVCR_ZA_SHIFT                                   1
#define SVCR_ZA_WIDTH                                   1

#define SVCR_SM                                         GENMASK(0, 0)
#define SVCR_SM_MASK                                    GENMASK(0, 0)
#define SVCR_SM_SHIFT                                   0
#define SVCR_SM_WIDTH                                   1

#define SVCR_RES0                                       (UL(0) | GENMASK_ULL(63, 2))
#define SVCR_RES1                                       (UL(0))
#define SVCR_UNKN                                       (UL(0))

#define HFGxTR_EL2_nAMAIR2_EL1                          GENMASK(63, 63)
#define HFGxTR_EL2_nAMAIR2_EL1_MASK                     GENMASK(63, 63)
#define HFGxTR_EL2_nAMAIR2_EL1_SHIFT                    63
#define HFGxTR_EL2_nAMAIR2_EL1_WIDTH                    1

#define HFGxTR_EL2_nMAIR2_EL1                           GENMASK(62, 62)
#define HFGxTR_EL2_nMAIR2_EL1_MASK                      GENMASK(62, 62)
#define HFGxTR_EL2_nMAIR2_EL1_SHIFT                     62
#define HFGxTR_EL2_nMAIR2_EL1_WIDTH                     1

#define HFGxTR_EL2_nS2POR_EL1                           GENMASK(61, 61)
#define HFGxTR_EL2_nS2POR_EL1_MASK                      GENMASK(61, 61)
#define HFGxTR_EL2_nS2POR_EL1_SHIFT                     61
#define HFGxTR_EL2_nS2POR_EL1_WIDTH                     1

#define HFGxTR_EL2_nPOR_EL1                             GENMASK(60, 60)
#define HFGxTR_EL2_nPOR_EL1_MASK                        GENMASK(60, 60)
#define HFGxTR_EL2_nPOR_EL1_SHIFT                       60
#define HFGxTR_EL2_nPOR_EL1_WIDTH                       1

#define HFGxTR_EL2_nPOR_EL0                             GENMASK(59, 59)
#define HFGxTR_EL2_nPOR_EL0_MASK                        GENMASK(59, 59)
#define HFGxTR_EL2_nPOR_EL0_SHIFT                       59
#define HFGxTR_EL2_nPOR_EL0_WIDTH                       1

#define HFGxTR_EL2_nPIR_EL1                             GENMASK(58, 58)
#define HFGxTR_EL2_nPIR_EL1_MASK                        GENMASK(58, 58)
#define HFGxTR_EL2_nPIR_EL1_SHIFT                       58
#define HFGxTR_EL2_nPIR_EL1_WIDTH                       1

#define HFGxTR_EL2_nPIRE0_EL1                           GENMASK(57, 57)
#define HFGxTR_EL2_nPIRE0_EL1_MASK                      GENMASK(57, 57)
#define HFGxTR_EL2_nPIRE0_EL1_SHIFT                     57
#define HFGxTR_EL2_nPIRE0_EL1_WIDTH                     1

#define HFGxTR_EL2_nRCWMASK_EL1                         GENMASK(56, 56)
#define HFGxTR_EL2_nRCWMASK_EL1_MASK                    GENMASK(56, 56)
#define HFGxTR_EL2_nRCWMASK_EL1_SHIFT                   56
#define HFGxTR_EL2_nRCWMASK_EL1_WIDTH                   1

#define HFGxTR_EL2_nTPIDR2_EL0                          GENMASK(55, 55)
#define HFGxTR_EL2_nTPIDR2_EL0_MASK                     GENMASK(55, 55)
#define HFGxTR_EL2_nTPIDR2_EL0_SHIFT                    55
#define HFGxTR_EL2_nTPIDR2_EL0_WIDTH                    1

#define HFGxTR_EL2_nSMPRI_EL1                           GENMASK(54, 54)
#define HFGxTR_EL2_nSMPRI_EL1_MASK                      GENMASK(54, 54)
#define HFGxTR_EL2_nSMPRI_EL1_SHIFT                     54
#define HFGxTR_EL2_nSMPRI_EL1_WIDTH                     1

#define HFGxTR_EL2_nGCS_EL1                             GENMASK(53, 53)
#define HFGxTR_EL2_nGCS_EL1_MASK                        GENMASK(53, 53)
#define HFGxTR_EL2_nGCS_EL1_SHIFT                       53
#define HFGxTR_EL2_nGCS_EL1_WIDTH                       1

#define HFGxTR_EL2_nGCS_EL0                             GENMASK(52, 52)
#define HFGxTR_EL2_nGCS_EL0_MASK                        GENMASK(52, 52)
#define HFGxTR_EL2_nGCS_EL0_SHIFT                       52
#define HFGxTR_EL2_nGCS_EL0_WIDTH                       1

#define HFGxTR_EL2_nACCDATA_EL1                         GENMASK(50, 50)
#define HFGxTR_EL2_nACCDATA_EL1_MASK                    GENMASK(50, 50)
#define HFGxTR_EL2_nACCDATA_EL1_SHIFT                   50
#define HFGxTR_EL2_nACCDATA_EL1_WIDTH                   1

#define HFGxTR_EL2_ERXADDR_EL1                          GENMASK(49, 49)
#define HFGxTR_EL2_ERXADDR_EL1_MASK                     GENMASK(49, 49)
#define HFGxTR_EL2_ERXADDR_EL1_SHIFT                    49
#define HFGxTR_EL2_ERXADDR_EL1_WIDTH                    1

#define HFGxTR_EL2_ERXPFGCDN_EL1                        GENMASK(48, 48)
#define HFGxTR_EL2_ERXPFGCDN_EL1_MASK                   GENMASK(48, 48)
#define HFGxTR_EL2_ERXPFGCDN_EL1_SHIFT                  48
#define HFGxTR_EL2_ERXPFGCDN_EL1_WIDTH                  1

#define HFGxTR_EL2_ERXPFGCTL_EL1                        GENMASK(47, 47)
#define HFGxTR_EL2_ERXPFGCTL_EL1_MASK                   GENMASK(47, 47)
#define HFGxTR_EL2_ERXPFGCTL_EL1_SHIFT                  47
#define HFGxTR_EL2_ERXPFGCTL_EL1_WIDTH                  1

#define HFGxTR_EL2_ERXPFGF_EL1                          GENMASK(46, 46)
#define HFGxTR_EL2_ERXPFGF_EL1_MASK                     GENMASK(46, 46)
#define HFGxTR_EL2_ERXPFGF_EL1_SHIFT                    46
#define HFGxTR_EL2_ERXPFGF_EL1_WIDTH                    1

#define HFGxTR_EL2_ERXMISCn_EL1                         GENMASK(45, 45)
#define HFGxTR_EL2_ERXMISCn_EL1_MASK                    GENMASK(45, 45)
#define HFGxTR_EL2_ERXMISCn_EL1_SHIFT                   45
#define HFGxTR_EL2_ERXMISCn_EL1_WIDTH                   1

#define HFGxTR_EL2_ERXSTATUS_EL1                        GENMASK(44, 44)
#define HFGxTR_EL2_ERXSTATUS_EL1_MASK                   GENMASK(44, 44)
#define HFGxTR_EL2_ERXSTATUS_EL1_SHIFT                  44
#define HFGxTR_EL2_ERXSTATUS_EL1_WIDTH                  1

#define HFGxTR_EL2_ERXCTLR_EL1                          GENMASK(43, 43)
#define HFGxTR_EL2_ERXCTLR_EL1_MASK                     GENMASK(43, 43)
#define HFGxTR_EL2_ERXCTLR_EL1_SHIFT                    43
#define HFGxTR_EL2_ERXCTLR_EL1_WIDTH                    1

#define HFGxTR_EL2_ERXFR_EL1                            GENMASK(42, 42)
#define HFGxTR_EL2_ERXFR_EL1_MASK                       GENMASK(42, 42)
#define HFGxTR_EL2_ERXFR_EL1_SHIFT                      42
#define HFGxTR_EL2_ERXFR_EL1_WIDTH                      1

#define HFGxTR_EL2_ERRSELR_EL1                          GENMASK(41, 41)
#define HFGxTR_EL2_ERRSELR_EL1_MASK                     GENMASK(41, 41)
#define HFGxTR_EL2_ERRSELR_EL1_SHIFT                    41
#define HFGxTR_EL2_ERRSELR_EL1_WIDTH                    1

#define HFGxTR_EL2_ERRIDR_EL1                           GENMASK(40, 40)
#define HFGxTR_EL2_ERRIDR_EL1_MASK                      GENMASK(40, 40)
#define HFGxTR_EL2_ERRIDR_EL1_SHIFT                     40
#define HFGxTR_EL2_ERRIDR_EL1_WIDTH                     1

#define HFGxTR_EL2_ICC_IGRPENn_EL1                      GENMASK(39, 39)
#define HFGxTR_EL2_ICC_IGRPENn_EL1_MASK                 GENMASK(39, 39)
#define HFGxTR_EL2_ICC_IGRPENn_EL1_SHIFT                39
#define HFGxTR_EL2_ICC_IGRPENn_EL1_WIDTH                1

#define HFGxTR_EL2_VBAR_EL1                             GENMASK(38, 38)
#define HFGxTR_EL2_VBAR_EL1_MASK                        GENMASK(38, 38)
#define HFGxTR_EL2_VBAR_EL1_SHIFT                       38
#define HFGxTR_EL2_VBAR_EL1_WIDTH                       1

#define HFGxTR_EL2_TTBR1_EL1                            GENMASK(37, 37)
#define HFGxTR_EL2_TTBR1_EL1_MASK                       GENMASK(37, 37)
#define HFGxTR_EL2_TTBR1_EL1_SHIFT                      37
#define HFGxTR_EL2_TTBR1_EL1_WIDTH                      1

#define HFGxTR_EL2_TTBR0_EL1                            GENMASK(36, 36)
#define HFGxTR_EL2_TTBR0_EL1_MASK                       GENMASK(36, 36)
#define HFGxTR_EL2_TTBR0_EL1_SHIFT                      36
#define HFGxTR_EL2_TTBR0_EL1_WIDTH                      1

#define HFGxTR_EL2_TPIDR_EL0                            GENMASK(35, 35)
#define HFGxTR_EL2_TPIDR_EL0_MASK                       GENMASK(35, 35)
#define HFGxTR_EL2_TPIDR_EL0_SHIFT                      35
#define HFGxTR_EL2_TPIDR_EL0_WIDTH                      1

#define HFGxTR_EL2_TPIDRRO_EL0                          GENMASK(34, 34)
#define HFGxTR_EL2_TPIDRRO_EL0_MASK                     GENMASK(34, 34)
#define HFGxTR_EL2_TPIDRRO_EL0_SHIFT                    34
#define HFGxTR_EL2_TPIDRRO_EL0_WIDTH                    1

#define HFGxTR_EL2_TPIDR_EL1                            GENMASK(33, 33)
#define HFGxTR_EL2_TPIDR_EL1_MASK                       GENMASK(33, 33)
#define HFGxTR_EL2_TPIDR_EL1_SHIFT                      33
#define HFGxTR_EL2_TPIDR_EL1_WIDTH                      1

#define HFGxTR_EL2_TCR_EL1                              GENMASK(32, 32)
#define HFGxTR_EL2_TCR_EL1_MASK                         GENMASK(32, 32)
#define HFGxTR_EL2_TCR_EL1_SHIFT                        32
#define HFGxTR_EL2_TCR_EL1_WIDTH                        1

#define HFGxTR_EL2_SCXTNUM_EL0                          GENMASK(31, 31)
#define HFGxTR_EL2_SCXTNUM_EL0_MASK                     GENMASK(31, 31)
#define HFGxTR_EL2_SCXTNUM_EL0_SHIFT                    31
#define HFGxTR_EL2_SCXTNUM_EL0_WIDTH                    1

#define HFGxTR_EL2_SCXTNUM_EL1                          GENMASK(30, 30)
#define HFGxTR_EL2_SCXTNUM_EL1_MASK                     GENMASK(30, 30)
#define HFGxTR_EL2_SCXTNUM_EL1_SHIFT                    30
#define HFGxTR_EL2_SCXTNUM_EL1_WIDTH                    1

#define HFGxTR_EL2_SCTLR_EL1                            GENMASK(29, 29)
#define HFGxTR_EL2_SCTLR_EL1_MASK                       GENMASK(29, 29)
#define HFGxTR_EL2_SCTLR_EL1_SHIFT                      29
#define HFGxTR_EL2_SCTLR_EL1_WIDTH                      1

#define HFGxTR_EL2_REVIDR_EL1                           GENMASK(28, 28)
#define HFGxTR_EL2_REVIDR_EL1_MASK                      GENMASK(28, 28)
#define HFGxTR_EL2_REVIDR_EL1_SHIFT                     28
#define HFGxTR_EL2_REVIDR_EL1_WIDTH                     1

#define HFGxTR_EL2_PAR_EL1                              GENMASK(27, 27)
#define HFGxTR_EL2_PAR_EL1_MASK                         GENMASK(27, 27)
#define HFGxTR_EL2_PAR_EL1_SHIFT                        27
#define HFGxTR_EL2_PAR_EL1_WIDTH                        1

#define HFGxTR_EL2_MPIDR_EL1                            GENMASK(26, 26)
#define HFGxTR_EL2_MPIDR_EL1_MASK                       GENMASK(26, 26)
#define HFGxTR_EL2_MPIDR_EL1_SHIFT                      26
#define HFGxTR_EL2_MPIDR_EL1_WIDTH                      1

#define HFGxTR_EL2_MIDR_EL1                             GENMASK(25, 25)
#define HFGxTR_EL2_MIDR_EL1_MASK                        GENMASK(25, 25)
#define HFGxTR_EL2_MIDR_EL1_SHIFT                       25
#define HFGxTR_EL2_MIDR_EL1_WIDTH                       1

#define HFGxTR_EL2_MAIR_EL1                             GENMASK(24, 24)
#define HFGxTR_EL2_MAIR_EL1_MASK                        GENMASK(24, 24)
#define HFGxTR_EL2_MAIR_EL1_SHIFT                       24
#define HFGxTR_EL2_MAIR_EL1_WIDTH                       1

#define HFGxTR_EL2_LORSA_EL1                            GENMASK(23, 23)
#define HFGxTR_EL2_LORSA_EL1_MASK                       GENMASK(23, 23)
#define HFGxTR_EL2_LORSA_EL1_SHIFT                      23
#define HFGxTR_EL2_LORSA_EL1_WIDTH                      1

#define HFGxTR_EL2_LORN_EL1                             GENMASK(22, 22)
#define HFGxTR_EL2_LORN_EL1_MASK                        GENMASK(22, 22)
#define HFGxTR_EL2_LORN_EL1_SHIFT                       22
#define HFGxTR_EL2_LORN_EL1_WIDTH                       1

#define HFGxTR_EL2_LORID_EL1                            GENMASK(21, 21)
#define HFGxTR_EL2_LORID_EL1_MASK                       GENMASK(21, 21)
#define HFGxTR_EL2_LORID_EL1_SHIFT                      21
#define HFGxTR_EL2_LORID_EL1_WIDTH                      1

#define HFGxTR_EL2_LOREA_EL1                            GENMASK(20, 20)
#define HFGxTR_EL2_LOREA_EL1_MASK                       GENMASK(20, 20)
#define HFGxTR_EL2_LOREA_EL1_SHIFT                      20
#define HFGxTR_EL2_LOREA_EL1_WIDTH                      1

#define HFGxTR_EL2_LORC_EL1                             GENMASK(19, 19)
#define HFGxTR_EL2_LORC_EL1_MASK                        GENMASK(19, 19)
#define HFGxTR_EL2_LORC_EL1_SHIFT                       19
#define HFGxTR_EL2_LORC_EL1_WIDTH                       1

#define HFGxTR_EL2_ISR_EL1                              GENMASK(18, 18)
#define HFGxTR_EL2_ISR_EL1_MASK                         GENMASK(18, 18)
#define HFGxTR_EL2_ISR_EL1_SHIFT                        18
#define HFGxTR_EL2_ISR_EL1_WIDTH                        1

#define HFGxTR_EL2_FAR_EL1                              GENMASK(17, 17)
#define HFGxTR_EL2_FAR_EL1_MASK                         GENMASK(17, 17)
#define HFGxTR_EL2_FAR_EL1_SHIFT                        17
#define HFGxTR_EL2_FAR_EL1_WIDTH                        1

#define HFGxTR_EL2_ESR_EL1                              GENMASK(16, 16)
#define HFGxTR_EL2_ESR_EL1_MASK                         GENMASK(16, 16)
#define HFGxTR_EL2_ESR_EL1_SHIFT                        16
#define HFGxTR_EL2_ESR_EL1_WIDTH                        1

#define HFGxTR_EL2_DCZID_EL0                            GENMASK(15, 15)
#define HFGxTR_EL2_DCZID_EL0_MASK                       GENMASK(15, 15)
#define HFGxTR_EL2_DCZID_EL0_SHIFT                      15
#define HFGxTR_EL2_DCZID_EL0_WIDTH                      1

#define HFGxTR_EL2_CTR_EL0                              GENMASK(14, 14)
#define HFGxTR_EL2_CTR_EL0_MASK                         GENMASK(14, 14)
#define HFGxTR_EL2_CTR_EL0_SHIFT                        14
#define HFGxTR_EL2_CTR_EL0_WIDTH                        1

#define HFGxTR_EL2_CSSELR_EL1                           GENMASK(13, 13)
#define HFGxTR_EL2_CSSELR_EL1_MASK                      GENMASK(13, 13)
#define HFGxTR_EL2_CSSELR_EL1_SHIFT                     13
#define HFGxTR_EL2_CSSELR_EL1_WIDTH                     1

#define HFGxTR_EL2_CPACR_EL1                            GENMASK(12, 12)
#define HFGxTR_EL2_CPACR_EL1_MASK                       GENMASK(12, 12)
#define HFGxTR_EL2_CPACR_EL1_SHIFT                      12
#define HFGxTR_EL2_CPACR_EL1_WIDTH                      1

#define HFGxTR_EL2_CONTEXTIDR_EL1                       GENMASK(11, 11)
#define HFGxTR_EL2_CONTEXTIDR_EL1_MASK                  GENMASK(11, 11)
#define HFGxTR_EL2_CONTEXTIDR_EL1_SHIFT                 11
#define HFGxTR_EL2_CONTEXTIDR_EL1_WIDTH                 1

#define HFGxTR_EL2_CLIDR_EL1                            GENMASK(10, 10)
#define HFGxTR_EL2_CLIDR_EL1_MASK                       GENMASK(10, 10)
#define HFGxTR_EL2_CLIDR_EL1_SHIFT                      10
#define HFGxTR_EL2_CLIDR_EL1_WIDTH                      1

#define HFGxTR_EL2_CCSIDR_EL1                           GENMASK(9, 9)
#define HFGxTR_EL2_CCSIDR_EL1_MASK                      GENMASK(9, 9)
#define HFGxTR_EL2_CCSIDR_EL1_SHIFT                     9
#define HFGxTR_EL2_CCSIDR_EL1_WIDTH                     1

#define HFGxTR_EL2_APIBKey                              GENMASK(8, 8)
#define HFGxTR_EL2_APIBKey_MASK                         GENMASK(8, 8)
#define HFGxTR_EL2_APIBKey_SHIFT                        8
#define HFGxTR_EL2_APIBKey_WIDTH                        1

#define HFGxTR_EL2_APIAKey                              GENMASK(7, 7)
#define HFGxTR_EL2_APIAKey_MASK                         GENMASK(7, 7)
#define HFGxTR_EL2_APIAKey_SHIFT                        7
#define HFGxTR_EL2_APIAKey_WIDTH                        1

#define HFGxTR_EL2_APGAKey                              GENMASK(6, 6)
#define HFGxTR_EL2_APGAKey_MASK                         GENMASK(6, 6)
#define HFGxTR_EL2_APGAKey_SHIFT                        6
#define HFGxTR_EL2_APGAKey_WIDTH                        1

#define HFGxTR_EL2_APDBKey                              GENMASK(5, 5)
#define HFGxTR_EL2_APDBKey_MASK                         GENMASK(5, 5)
#define HFGxTR_EL2_APDBKey_SHIFT                        5
#define HFGxTR_EL2_APDBKey_WIDTH                        1

#define HFGxTR_EL2_APDAKey                              GENMASK(4, 4)
#define HFGxTR_EL2_APDAKey_MASK                         GENMASK(4, 4)
#define HFGxTR_EL2_APDAKey_SHIFT                        4
#define HFGxTR_EL2_APDAKey_WIDTH                        1

#define HFGxTR_EL2_AMAIR_EL1                            GENMASK(3, 3)
#define HFGxTR_EL2_AMAIR_EL1_MASK                       GENMASK(3, 3)
#define HFGxTR_EL2_AMAIR_EL1_SHIFT                      3
#define HFGxTR_EL2_AMAIR_EL1_WIDTH                      1

#define HFGxTR_EL2_AIDR_EL1                             GENMASK(2, 2)
#define HFGxTR_EL2_AIDR_EL1_MASK                        GENMASK(2, 2)
#define HFGxTR_EL2_AIDR_EL1_SHIFT                       2
#define HFGxTR_EL2_AIDR_EL1_WIDTH                       1

#define HFGxTR_EL2_AFSR1_EL1                            GENMASK(1, 1)
#define HFGxTR_EL2_AFSR1_EL1_MASK                       GENMASK(1, 1)
#define HFGxTR_EL2_AFSR1_EL1_SHIFT                      1
#define HFGxTR_EL2_AFSR1_EL1_WIDTH                      1

#define HFGxTR_EL2_AFSR0_EL1                            GENMASK(0, 0)
#define HFGxTR_EL2_AFSR0_EL1_MASK                       GENMASK(0, 0)
#define HFGxTR_EL2_AFSR0_EL1_SHIFT                      0
#define HFGxTR_EL2_AFSR0_EL1_WIDTH                      1

#define HFGxTR_EL2_RES0                                 (UL(0) | GENMASK_ULL(51, 51))
#define HFGxTR_EL2_RES1                                 (UL(0))
#define HFGxTR_EL2_UNKN                                 (UL(0))

#define REG_HFGRTR_EL2                                  S3_4_C1_C1_4
#define SYS_HFGRTR_EL2                                  sys_reg(3, 4, 1, 1, 4)
#define SYS_HFGRTR_EL2_Op0                              3
#define SYS_HFGRTR_EL2_Op1                              4
#define SYS_HFGRTR_EL2_CRn                              1
#define SYS_HFGRTR_EL2_CRm                              1
#define SYS_HFGRTR_EL2_Op2                              4

/* For HFGRTR_EL2 fields see HFGxTR_EL2 */

#define REG_HFGWTR_EL2                                  S3_4_C1_C1_5
#define SYS_HFGWTR_EL2                                  sys_reg(3, 4, 1, 1, 5)
#define SYS_HFGWTR_EL2_Op0                              3
#define SYS_HFGWTR_EL2_Op1                              4
#define SYS_HFGWTR_EL2_CRn                              1
#define SYS_HFGWTR_EL2_CRm                              1
#define SYS_HFGWTR_EL2_Op2                              5

/* For HFGWTR_EL2 fields see HFGxTR_EL2 */

#define REG_HFGITR_EL2                                  S3_4_C1_C1_6
#define SYS_HFGITR_EL2                                  sys_reg(3, 4, 1, 1, 6)
#define SYS_HFGITR_EL2_Op0                              3
#define SYS_HFGITR_EL2_Op1                              4
#define SYS_HFGITR_EL2_CRn                              1
#define SYS_HFGITR_EL2_CRm                              1
#define SYS_HFGITR_EL2_Op2                              6

#define HFGITR_EL2_COSPRCTX                             GENMASK(60, 60)
#define HFGITR_EL2_COSPRCTX_MASK                        GENMASK(60, 60)
#define HFGITR_EL2_COSPRCTX_SHIFT                       60
#define HFGITR_EL2_COSPRCTX_WIDTH                       1

#define HFGITR_EL2_nGCSEPP                              GENMASK(59, 59)
#define HFGITR_EL2_nGCSEPP_MASK                         GENMASK(59, 59)
#define HFGITR_EL2_nGCSEPP_SHIFT                        59
#define HFGITR_EL2_nGCSEPP_WIDTH                        1

#define HFGITR_EL2_nGCSSTR_EL1                          GENMASK(58, 58)
#define HFGITR_EL2_nGCSSTR_EL1_MASK                     GENMASK(58, 58)
#define HFGITR_EL2_nGCSSTR_EL1_SHIFT                    58
#define HFGITR_EL2_nGCSSTR_EL1_WIDTH                    1

#define HFGITR_EL2_nGCSPUSHM_EL1                        GENMASK(57, 57)
#define HFGITR_EL2_nGCSPUSHM_EL1_MASK                   GENMASK(57, 57)
#define HFGITR_EL2_nGCSPUSHM_EL1_SHIFT                  57
#define HFGITR_EL2_nGCSPUSHM_EL1_WIDTH                  1

#define HFGITR_EL2_nBRBIALL                             GENMASK(56, 56)
#define HFGITR_EL2_nBRBIALL_MASK                        GENMASK(56, 56)
#define HFGITR_EL2_nBRBIALL_SHIFT                       56
#define HFGITR_EL2_nBRBIALL_WIDTH                       1

#define HFGITR_EL2_nBRBINJ                              GENMASK(55, 55)
#define HFGITR_EL2_nBRBINJ_MASK                         GENMASK(55, 55)
#define HFGITR_EL2_nBRBINJ_SHIFT                        55
#define HFGITR_EL2_nBRBINJ_WIDTH                        1

#define HFGITR_EL2_DCCVAC                               GENMASK(54, 54)
#define HFGITR_EL2_DCCVAC_MASK                          GENMASK(54, 54)
#define HFGITR_EL2_DCCVAC_SHIFT                         54
#define HFGITR_EL2_DCCVAC_WIDTH                         1

#define HFGITR_EL2_SVC_EL1                              GENMASK(53, 53)
#define HFGITR_EL2_SVC_EL1_MASK                         GENMASK(53, 53)
#define HFGITR_EL2_SVC_EL1_SHIFT                        53
#define HFGITR_EL2_SVC_EL1_WIDTH                        1

#define HFGITR_EL2_SVC_EL0                              GENMASK(52, 52)
#define HFGITR_EL2_SVC_EL0_MASK                         GENMASK(52, 52)
#define HFGITR_EL2_SVC_EL0_SHIFT                        52
#define HFGITR_EL2_SVC_EL0_WIDTH                        1

#define HFGITR_EL2_ERET                                 GENMASK(51, 51)
#define HFGITR_EL2_ERET_MASK                            GENMASK(51, 51)
#define HFGITR_EL2_ERET_SHIFT                           51
#define HFGITR_EL2_ERET_WIDTH                           1

#define HFGITR_EL2_CPPRCTX                              GENMASK(50, 50)
#define HFGITR_EL2_CPPRCTX_MASK                         GENMASK(50, 50)
#define HFGITR_EL2_CPPRCTX_SHIFT                        50
#define HFGITR_EL2_CPPRCTX_WIDTH                        1

#define HFGITR_EL2_DVPRCTX                              GENMASK(49, 49)
#define HFGITR_EL2_DVPRCTX_MASK                         GENMASK(49, 49)
#define HFGITR_EL2_DVPRCTX_SHIFT                        49
#define HFGITR_EL2_DVPRCTX_WIDTH                        1

#define HFGITR_EL2_CFPRCTX                              GENMASK(48, 48)
#define HFGITR_EL2_CFPRCTX_MASK                         GENMASK(48, 48)
#define HFGITR_EL2_CFPRCTX_SHIFT                        48
#define HFGITR_EL2_CFPRCTX_WIDTH                        1

#define HFGITR_EL2_TLBIVAALE1                           GENMASK(47, 47)
#define HFGITR_EL2_TLBIVAALE1_MASK                      GENMASK(47, 47)
#define HFGITR_EL2_TLBIVAALE1_SHIFT                     47
#define HFGITR_EL2_TLBIVAALE1_WIDTH                     1

#define HFGITR_EL2_TLBIVALE1                            GENMASK(46, 46)
#define HFGITR_EL2_TLBIVALE1_MASK                       GENMASK(46, 46)
#define HFGITR_EL2_TLBIVALE1_SHIFT                      46
#define HFGITR_EL2_TLBIVALE1_WIDTH                      1

#define HFGITR_EL2_TLBIVAAE1                            GENMASK(45, 45)
#define HFGITR_EL2_TLBIVAAE1_MASK                       GENMASK(45, 45)
#define HFGITR_EL2_TLBIVAAE1_SHIFT                      45
#define HFGITR_EL2_TLBIVAAE1_WIDTH                      1

#define HFGITR_EL2_TLBIASIDE1                           GENMASK(44, 44)
#define HFGITR_EL2_TLBIASIDE1_MASK                      GENMASK(44, 44)
#define HFGITR_EL2_TLBIASIDE1_SHIFT                     44
#define HFGITR_EL2_TLBIASIDE1_WIDTH                     1

#define HFGITR_EL2_TLBIVAE1                             GENMASK(43, 43)
#define HFGITR_EL2_TLBIVAE1_MASK                        GENMASK(43, 43)
#define HFGITR_EL2_TLBIVAE1_SHIFT                       43
#define HFGITR_EL2_TLBIVAE1_WIDTH                       1

#define HFGITR_EL2_TLBIVMALLE1                          GENMASK(42, 42)
#define HFGITR_EL2_TLBIVMALLE1_MASK                     GENMASK(42, 42)
#define HFGITR_EL2_TLBIVMALLE1_SHIFT                    42
#define HFGITR_EL2_TLBIVMALLE1_WIDTH                    1

#define HFGITR_EL2_TLBIRVAALE1                          GENMASK(41, 41)
#define HFGITR_EL2_TLBIRVAALE1_MASK                     GENMASK(41, 41)
#define HFGITR_EL2_TLBIRVAALE1_SHIFT                    41
#define HFGITR_EL2_TLBIRVAALE1_WIDTH                    1

#define HFGITR_EL2_TLBIRVALE1                           GENMASK(40, 40)
#define HFGITR_EL2_TLBIRVALE1_MASK                      GENMASK(40, 40)
#define HFGITR_EL2_TLBIRVALE1_SHIFT                     40
#define HFGITR_EL2_TLBIRVALE1_WIDTH                     1

#define HFGITR_EL2_TLBIRVAAE1                           GENMASK(39, 39)
#define HFGITR_EL2_TLBIRVAAE1_MASK                      GENMASK(39, 39)
#define HFGITR_EL2_TLBIRVAAE1_SHIFT                     39
#define HFGITR_EL2_TLBIRVAAE1_WIDTH                     1

#define HFGITR_EL2_TLBIRVAE1                            GENMASK(38, 38)
#define HFGITR_EL2_TLBIRVAE1_MASK                       GENMASK(38, 38)
#define HFGITR_EL2_TLBIRVAE1_SHIFT                      38
#define HFGITR_EL2_TLBIRVAE1_WIDTH                      1

#define HFGITR_EL2_TLBIRVAALE1IS                        GENMASK(37, 37)
#define HFGITR_EL2_TLBIRVAALE1IS_MASK                   GENMASK(37, 37)
#define HFGITR_EL2_TLBIRVAALE1IS_SHIFT                  37
#define HFGITR_EL2_TLBIRVAALE1IS_WIDTH                  1

#define HFGITR_EL2_TLBIRVALE1IS                         GENMASK(36, 36)
#define HFGITR_EL2_TLBIRVALE1IS_MASK                    GENMASK(36, 36)
#define HFGITR_EL2_TLBIRVALE1IS_SHIFT                   36
#define HFGITR_EL2_TLBIRVALE1IS_WIDTH                   1

#define HFGITR_EL2_TLBIRVAAE1IS                         GENMASK(35, 35)
#define HFGITR_EL2_TLBIRVAAE1IS_MASK                    GENMASK(35, 35)
#define HFGITR_EL2_TLBIRVAAE1IS_SHIFT                   35
#define HFGITR_EL2_TLBIRVAAE1IS_WIDTH                   1

#define HFGITR_EL2_TLBIRVAE1IS                          GENMASK(34, 34)
#define HFGITR_EL2_TLBIRVAE1IS_MASK                     GENMASK(34, 34)
#define HFGITR_EL2_TLBIRVAE1IS_SHIFT                    34
#define HFGITR_EL2_TLBIRVAE1IS_WIDTH                    1

#define HFGITR_EL2_TLBIVAALE1IS                         GENMASK(33, 33)
#define HFGITR_EL2_TLBIVAALE1IS_MASK                    GENMASK(33, 33)
#define HFGITR_EL2_TLBIVAALE1IS_SHIFT                   33
#define HFGITR_EL2_TLBIVAALE1IS_WIDTH                   1

#define HFGITR_EL2_TLBIVALE1IS                          GENMASK(32, 32)
#define HFGITR_EL2_TLBIVALE1IS_MASK                     GENMASK(32, 32)
#define HFGITR_EL2_TLBIVALE1IS_SHIFT                    32
#define HFGITR_EL2_TLBIVALE1IS_WIDTH                    1

#define HFGITR_EL2_TLBIVAAE1IS                          GENMASK(31, 31)
#define HFGITR_EL2_TLBIVAAE1IS_MASK                     GENMASK(31, 31)
#define HFGITR_EL2_TLBIVAAE1IS_SHIFT                    31
#define HFGITR_EL2_TLBIVAAE1IS_WIDTH                    1

#define HFGITR_EL2_TLBIASIDE1IS                         GENMASK(30, 30)
#define HFGITR_EL2_TLBIASIDE1IS_MASK                    GENMASK(30, 30)
#define HFGITR_EL2_TLBIASIDE1IS_SHIFT                   30
#define HFGITR_EL2_TLBIASIDE1IS_WIDTH                   1

#define HFGITR_EL2_TLBIVAE1IS                           GENMASK(29, 29)
#define HFGITR_EL2_TLBIVAE1IS_MASK                      GENMASK(29, 29)
#define HFGITR_EL2_TLBIVAE1IS_SHIFT                     29
#define HFGITR_EL2_TLBIVAE1IS_WIDTH                     1

#define HFGITR_EL2_TLBIVMALLE1IS                        GENMASK(28, 28)
#define HFGITR_EL2_TLBIVMALLE1IS_MASK                   GENMASK(28, 28)
#define HFGITR_EL2_TLBIVMALLE1IS_SHIFT                  28
#define HFGITR_EL2_TLBIVMALLE1IS_WIDTH                  1

#define HFGITR_EL2_TLBIRVAALE1OS                        GENMASK(27, 27)
#define HFGITR_EL2_TLBIRVAALE1OS_MASK                   GENMASK(27, 27)
#define HFGITR_EL2_TLBIRVAALE1OS_SHIFT                  27
#define HFGITR_EL2_TLBIRVAALE1OS_WIDTH                  1

#define HFGITR_EL2_TLBIRVALE1OS                         GENMASK(26, 26)
#define HFGITR_EL2_TLBIRVALE1OS_MASK                    GENMASK(26, 26)
#define HFGITR_EL2_TLBIRVALE1OS_SHIFT                   26
#define HFGITR_EL2_TLBIRVALE1OS_WIDTH                   1

#define HFGITR_EL2_TLBIRVAAE1OS                         GENMASK(25, 25)
#define HFGITR_EL2_TLBIRVAAE1OS_MASK                    GENMASK(25, 25)
#define HFGITR_EL2_TLBIRVAAE1OS_SHIFT                   25
#define HFGITR_EL2_TLBIRVAAE1OS_WIDTH                   1

#define HFGITR_EL2_TLBIRVAE1OS                          GENMASK(24, 24)
#define HFGITR_EL2_TLBIRVAE1OS_MASK                     GENMASK(24, 24)
#define HFGITR_EL2_TLBIRVAE1OS_SHIFT                    24
#define HFGITR_EL2_TLBIRVAE1OS_WIDTH                    1

#define HFGITR_EL2_TLBIVAALE1OS                         GENMASK(23, 23)
#define HFGITR_EL2_TLBIVAALE1OS_MASK                    GENMASK(23, 23)
#define HFGITR_EL2_TLBIVAALE1OS_SHIFT                   23
#define HFGITR_EL2_TLBIVAALE1OS_WIDTH                   1

#define HFGITR_EL2_TLBIVALE1OS                          GENMASK(22, 22)
#define HFGITR_EL2_TLBIVALE1OS_MASK                     GENMASK(22, 22)
#define HFGITR_EL2_TLBIVALE1OS_SHIFT                    22
#define HFGITR_EL2_TLBIVALE1OS_WIDTH                    1

#define HFGITR_EL2_TLBIVAAE1OS                          GENMASK(21, 21)
#define HFGITR_EL2_TLBIVAAE1OS_MASK                     GENMASK(21, 21)
#define HFGITR_EL2_TLBIVAAE1OS_SHIFT                    21
#define HFGITR_EL2_TLBIVAAE1OS_WIDTH                    1

#define HFGITR_EL2_TLBIASIDE1OS                         GENMASK(20, 20)
#define HFGITR_EL2_TLBIASIDE1OS_MASK                    GENMASK(20, 20)
#define HFGITR_EL2_TLBIASIDE1OS_SHIFT                   20
#define HFGITR_EL2_TLBIASIDE1OS_WIDTH                   1

#define HFGITR_EL2_TLBIVAE1OS                           GENMASK(19, 19)
#define HFGITR_EL2_TLBIVAE1OS_MASK                      GENMASK(19, 19)
#define HFGITR_EL2_TLBIVAE1OS_SHIFT                     19
#define HFGITR_EL2_TLBIVAE1OS_WIDTH                     1

#define HFGITR_EL2_TLBIVMALLE1OS                        GENMASK(18, 18)
#define HFGITR_EL2_TLBIVMALLE1OS_MASK                   GENMASK(18, 18)
#define HFGITR_EL2_TLBIVMALLE1OS_SHIFT                  18
#define HFGITR_EL2_TLBIVMALLE1OS_WIDTH                  1

#define HFGITR_EL2_ATS1E1WP                             GENMASK(17, 17)
#define HFGITR_EL2_ATS1E1WP_MASK                        GENMASK(17, 17)
#define HFGITR_EL2_ATS1E1WP_SHIFT                       17
#define HFGITR_EL2_ATS1E1WP_WIDTH                       1

#define HFGITR_EL2_ATS1E1RP                             GENMASK(16, 16)
#define HFGITR_EL2_ATS1E1RP_MASK                        GENMASK(16, 16)
#define HFGITR_EL2_ATS1E1RP_SHIFT                       16
#define HFGITR_EL2_ATS1E1RP_WIDTH                       1

#define HFGITR_EL2_ATS1E0W                              GENMASK(15, 15)
#define HFGITR_EL2_ATS1E0W_MASK                         GENMASK(15, 15)
#define HFGITR_EL2_ATS1E0W_SHIFT                        15
#define HFGITR_EL2_ATS1E0W_WIDTH                        1

#define HFGITR_EL2_ATS1E0R                              GENMASK(14, 14)
#define HFGITR_EL2_ATS1E0R_MASK                         GENMASK(14, 14)
#define HFGITR_EL2_ATS1E0R_SHIFT                        14
#define HFGITR_EL2_ATS1E0R_WIDTH                        1

#define HFGITR_EL2_ATS1E1W                              GENMASK(13, 13)
#define HFGITR_EL2_ATS1E1W_MASK                         GENMASK(13, 13)
#define HFGITR_EL2_ATS1E1W_SHIFT                        13
#define HFGITR_EL2_ATS1E1W_WIDTH                        1

#define HFGITR_EL2_ATS1E1R                              GENMASK(12, 12)
#define HFGITR_EL2_ATS1E1R_MASK                         GENMASK(12, 12)
#define HFGITR_EL2_ATS1E1R_SHIFT                        12
#define HFGITR_EL2_ATS1E1R_WIDTH                        1

#define HFGITR_EL2_DCZVA                                GENMASK(11, 11)
#define HFGITR_EL2_DCZVA_MASK                           GENMASK(11, 11)
#define HFGITR_EL2_DCZVA_SHIFT                          11
#define HFGITR_EL2_DCZVA_WIDTH                          1

#define HFGITR_EL2_DCCIVAC                              GENMASK(10, 10)
#define HFGITR_EL2_DCCIVAC_MASK                         GENMASK(10, 10)
#define HFGITR_EL2_DCCIVAC_SHIFT                        10
#define HFGITR_EL2_DCCIVAC_WIDTH                        1

#define HFGITR_EL2_DCCVADP                              GENMASK(9, 9)
#define HFGITR_EL2_DCCVADP_MASK                         GENMASK(9, 9)
#define HFGITR_EL2_DCCVADP_SHIFT                        9
#define HFGITR_EL2_DCCVADP_WIDTH                        1

#define HFGITR_EL2_DCCVAP                               GENMASK(8, 8)
#define HFGITR_EL2_DCCVAP_MASK                          GENMASK(8, 8)
#define HFGITR_EL2_DCCVAP_SHIFT                         8
#define HFGITR_EL2_DCCVAP_WIDTH                         1

#define HFGITR_EL2_DCCVAU                               GENMASK(7, 7)
#define HFGITR_EL2_DCCVAU_MASK                          GENMASK(7, 7)
#define HFGITR_EL2_DCCVAU_SHIFT                         7
#define HFGITR_EL2_DCCVAU_WIDTH                         1

#define HFGITR_EL2_DCCISW                               GENMASK(6, 6)
#define HFGITR_EL2_DCCISW_MASK                          GENMASK(6, 6)
#define HFGITR_EL2_DCCISW_SHIFT                         6
#define HFGITR_EL2_DCCISW_WIDTH                         1

#define HFGITR_EL2_DCCSW                                GENMASK(5, 5)
#define HFGITR_EL2_DCCSW_MASK                           GENMASK(5, 5)
#define HFGITR_EL2_DCCSW_SHIFT                          5
#define HFGITR_EL2_DCCSW_WIDTH                          1

#define HFGITR_EL2_DCISW                                GENMASK(4, 4)
#define HFGITR_EL2_DCISW_MASK                           GENMASK(4, 4)
#define HFGITR_EL2_DCISW_SHIFT                          4
#define HFGITR_EL2_DCISW_WIDTH                          1

#define HFGITR_EL2_DCIVAC                               GENMASK(3, 3)
#define HFGITR_EL2_DCIVAC_MASK                          GENMASK(3, 3)
#define HFGITR_EL2_DCIVAC_SHIFT                         3
#define HFGITR_EL2_DCIVAC_WIDTH                         1

#define HFGITR_EL2_ICIVAU                               GENMASK(2, 2)
#define HFGITR_EL2_ICIVAU_MASK                          GENMASK(2, 2)
#define HFGITR_EL2_ICIVAU_SHIFT                         2
#define HFGITR_EL2_ICIVAU_WIDTH                         1

#define HFGITR_EL2_ICIALLU                              GENMASK(1, 1)
#define HFGITR_EL2_ICIALLU_MASK                         GENMASK(1, 1)
#define HFGITR_EL2_ICIALLU_SHIFT                        1
#define HFGITR_EL2_ICIALLU_WIDTH                        1

#define HFGITR_EL2_ICIALLUIS                            GENMASK(0, 0)
#define HFGITR_EL2_ICIALLUIS_MASK                       GENMASK(0, 0)
#define HFGITR_EL2_ICIALLUIS_SHIFT                      0
#define HFGITR_EL2_ICIALLUIS_WIDTH                      1

#define HFGITR_EL2_RES0                                 (UL(0) | GENMASK_ULL(63, 61))
#define HFGITR_EL2_RES1                                 (UL(0))
#define HFGITR_EL2_UNKN                                 (UL(0))

#define REG_HDFGRTR_EL2                                 S3_4_C3_C1_4
#define SYS_HDFGRTR_EL2                                 sys_reg(3, 4, 3, 1, 4)
#define SYS_HDFGRTR_EL2_Op0                             3
#define SYS_HDFGRTR_EL2_Op1                             4
#define SYS_HDFGRTR_EL2_CRn                             3
#define SYS_HDFGRTR_EL2_CRm                             1
#define SYS_HDFGRTR_EL2_Op2                             4

#define HDFGRTR_EL2_PMBIDR_EL1                          GENMASK(63, 63)
#define HDFGRTR_EL2_PMBIDR_EL1_MASK                     GENMASK(63, 63)
#define HDFGRTR_EL2_PMBIDR_EL1_SHIFT                    63
#define HDFGRTR_EL2_PMBIDR_EL1_WIDTH                    1

#define HDFGRTR_EL2_nPMSNEVFR_EL1                       GENMASK(62, 62)
#define HDFGRTR_EL2_nPMSNEVFR_EL1_MASK                  GENMASK(62, 62)
#define HDFGRTR_EL2_nPMSNEVFR_EL1_SHIFT                 62
#define HDFGRTR_EL2_nPMSNEVFR_EL1_WIDTH                 1

#define HDFGRTR_EL2_nBRBDATA                            GENMASK(61, 61)
#define HDFGRTR_EL2_nBRBDATA_MASK                       GENMASK(61, 61)
#define HDFGRTR_EL2_nBRBDATA_SHIFT                      61
#define HDFGRTR_EL2_nBRBDATA_WIDTH                      1

#define HDFGRTR_EL2_nBRBCTL                             GENMASK(60, 60)
#define HDFGRTR_EL2_nBRBCTL_MASK                        GENMASK(60, 60)
#define HDFGRTR_EL2_nBRBCTL_SHIFT                       60
#define HDFGRTR_EL2_nBRBCTL_WIDTH                       1

#define HDFGRTR_EL2_nBRBIDR                             GENMASK(59, 59)
#define HDFGRTR_EL2_nBRBIDR_MASK                        GENMASK(59, 59)
#define HDFGRTR_EL2_nBRBIDR_SHIFT                       59
#define HDFGRTR_EL2_nBRBIDR_WIDTH                       1

#define HDFGRTR_EL2_PMCEIDn_EL0                         GENMASK(58, 58)
#define HDFGRTR_EL2_PMCEIDn_EL0_MASK                    GENMASK(58, 58)
#define HDFGRTR_EL2_PMCEIDn_EL0_SHIFT                   58
#define HDFGRTR_EL2_PMCEIDn_EL0_WIDTH                   1

#define HDFGRTR_EL2_PMUSERENR_EL0                       GENMASK(57, 57)
#define HDFGRTR_EL2_PMUSERENR_EL0_MASK                  GENMASK(57, 57)
#define HDFGRTR_EL2_PMUSERENR_EL0_SHIFT                 57
#define HDFGRTR_EL2_PMUSERENR_EL0_WIDTH                 1

#define HDFGRTR_EL2_TRBTRG_EL1                          GENMASK(56, 56)
#define HDFGRTR_EL2_TRBTRG_EL1_MASK                     GENMASK(56, 56)
#define HDFGRTR_EL2_TRBTRG_EL1_SHIFT                    56
#define HDFGRTR_EL2_TRBTRG_EL1_WIDTH                    1

#define HDFGRTR_EL2_TRBSR_EL1                           GENMASK(55, 55)
#define HDFGRTR_EL2_TRBSR_EL1_MASK                      GENMASK(55, 55)
#define HDFGRTR_EL2_TRBSR_EL1_SHIFT                     55
#define HDFGRTR_EL2_TRBSR_EL1_WIDTH                     1

#define HDFGRTR_EL2_TRBPTR_EL1                          GENMASK(54, 54)
#define HDFGRTR_EL2_TRBPTR_EL1_MASK                     GENMASK(54, 54)
#define HDFGRTR_EL2_TRBPTR_EL1_SHIFT                    54
#define HDFGRTR_EL2_TRBPTR_EL1_WIDTH                    1

#define HDFGRTR_EL2_TRBMAR_EL1                          GENMASK(53, 53)
#define HDFGRTR_EL2_TRBMAR_EL1_MASK                     GENMASK(53, 53)
#define HDFGRTR_EL2_TRBMAR_EL1_SHIFT                    53
#define HDFGRTR_EL2_TRBMAR_EL1_WIDTH                    1

#define HDFGRTR_EL2_TRBLIMITR_EL1                       GENMASK(52, 52)
#define HDFGRTR_EL2_TRBLIMITR_EL1_MASK                  GENMASK(52, 52)
#define HDFGRTR_EL2_TRBLIMITR_EL1_SHIFT                 52
#define HDFGRTR_EL2_TRBLIMITR_EL1_WIDTH                 1

#define HDFGRTR_EL2_TRBIDR_EL1                          GENMASK(51, 51)
#define HDFGRTR_EL2_TRBIDR_EL1_MASK                     GENMASK(51, 51)
#define HDFGRTR_EL2_TRBIDR_EL1_SHIFT                    51
#define HDFGRTR_EL2_TRBIDR_EL1_WIDTH                    1

#define HDFGRTR_EL2_TRBBASER_EL1                        GENMASK(50, 50)
#define HDFGRTR_EL2_TRBBASER_EL1_MASK                   GENMASK(50, 50)
#define HDFGRTR_EL2_TRBBASER_EL1_SHIFT                  50
#define HDFGRTR_EL2_TRBBASER_EL1_WIDTH                  1

#define HDFGRTR_EL2_TRCVICTLR                           GENMASK(48, 48)
#define HDFGRTR_EL2_TRCVICTLR_MASK                      GENMASK(48, 48)
#define HDFGRTR_EL2_TRCVICTLR_SHIFT                     48
#define HDFGRTR_EL2_TRCVICTLR_WIDTH                     1

#define HDFGRTR_EL2_TRCSTATR                            GENMASK(47, 47)
#define HDFGRTR_EL2_TRCSTATR_MASK                       GENMASK(47, 47)
#define HDFGRTR_EL2_TRCSTATR_SHIFT                      47
#define HDFGRTR_EL2_TRCSTATR_WIDTH                      1

#define HDFGRTR_EL2_TRCSSCSRn                           GENMASK(46, 46)
#define HDFGRTR_EL2_TRCSSCSRn_MASK                      GENMASK(46, 46)
#define HDFGRTR_EL2_TRCSSCSRn_SHIFT                     46
#define HDFGRTR_EL2_TRCSSCSRn_WIDTH                     1

#define HDFGRTR_EL2_TRCSEQSTR                           GENMASK(45, 45)
#define HDFGRTR_EL2_TRCSEQSTR_MASK                      GENMASK(45, 45)
#define HDFGRTR_EL2_TRCSEQSTR_SHIFT                     45
#define HDFGRTR_EL2_TRCSEQSTR_WIDTH                     1

#define HDFGRTR_EL2_TRCPRGCTLR                          GENMASK(44, 44)
#define HDFGRTR_EL2_TRCPRGCTLR_MASK                     GENMASK(44, 44)
#define HDFGRTR_EL2_TRCPRGCTLR_SHIFT                    44
#define HDFGRTR_EL2_TRCPRGCTLR_WIDTH                    1

#define HDFGRTR_EL2_TRCOSLSR                            GENMASK(43, 43)
#define HDFGRTR_EL2_TRCOSLSR_MASK                       GENMASK(43, 43)
#define HDFGRTR_EL2_TRCOSLSR_SHIFT                      43
#define HDFGRTR_EL2_TRCOSLSR_WIDTH                      1

#define HDFGRTR_EL2_TRCIMSPECn                          GENMASK(41, 41)
#define HDFGRTR_EL2_TRCIMSPECn_MASK                     GENMASK(41, 41)
#define HDFGRTR_EL2_TRCIMSPECn_SHIFT                    41
#define HDFGRTR_EL2_TRCIMSPECn_WIDTH                    1

#define HDFGRTR_EL2_TRCID                               GENMASK(40, 40)
#define HDFGRTR_EL2_TRCID_MASK                          GENMASK(40, 40)
#define HDFGRTR_EL2_TRCID_SHIFT                         40
#define HDFGRTR_EL2_TRCID_WIDTH                         1

#define HDFGRTR_EL2_TRCCNTVRn                           GENMASK(37, 37)
#define HDFGRTR_EL2_TRCCNTVRn_MASK                      GENMASK(37, 37)
#define HDFGRTR_EL2_TRCCNTVRn_SHIFT                     37
#define HDFGRTR_EL2_TRCCNTVRn_WIDTH                     1

#define HDFGRTR_EL2_TRCCLAIM                            GENMASK(36, 36)
#define HDFGRTR_EL2_TRCCLAIM_MASK                       GENMASK(36, 36)
#define HDFGRTR_EL2_TRCCLAIM_SHIFT                      36
#define HDFGRTR_EL2_TRCCLAIM_WIDTH                      1

#define HDFGRTR_EL2_TRCAUXCTLR                          GENMASK(35, 35)
#define HDFGRTR_EL2_TRCAUXCTLR_MASK                     GENMASK(35, 35)
#define HDFGRTR_EL2_TRCAUXCTLR_SHIFT                    35
#define HDFGRTR_EL2_TRCAUXCTLR_WIDTH                    1

#define HDFGRTR_EL2_TRCAUTHSTATUS                       GENMASK(34, 34)
#define HDFGRTR_EL2_TRCAUTHSTATUS_MASK                  GENMASK(34, 34)
#define HDFGRTR_EL2_TRCAUTHSTATUS_SHIFT                 34
#define HDFGRTR_EL2_TRCAUTHSTATUS_WIDTH                 1

#define HDFGRTR_EL2_TRC                                 GENMASK(33, 33)
#define HDFGRTR_EL2_TRC_MASK                            GENMASK(33, 33)
#define HDFGRTR_EL2_TRC_SHIFT                           33
#define HDFGRTR_EL2_TRC_WIDTH                           1

#define HDFGRTR_EL2_PMSLATFR_EL1                        GENMASK(32, 32)
#define HDFGRTR_EL2_PMSLATFR_EL1_MASK                   GENMASK(32, 32)
#define HDFGRTR_EL2_PMSLATFR_EL1_SHIFT                  32
#define HDFGRTR_EL2_PMSLATFR_EL1_WIDTH                  1

#define HDFGRTR_EL2_PMSIRR_EL1                          GENMASK(31, 31)
#define HDFGRTR_EL2_PMSIRR_EL1_MASK                     GENMASK(31, 31)
#define HDFGRTR_EL2_PMSIRR_EL1_SHIFT                    31
#define HDFGRTR_EL2_PMSIRR_EL1_WIDTH                    1

#define HDFGRTR_EL2_PMSIDR_EL1                          GENMASK(30, 30)
#define HDFGRTR_EL2_PMSIDR_EL1_MASK                     GENMASK(30, 30)
#define HDFGRTR_EL2_PMSIDR_EL1_SHIFT                    30
#define HDFGRTR_EL2_PMSIDR_EL1_WIDTH                    1

#define HDFGRTR_EL2_PMSICR_EL1                          GENMASK(29, 29)
#define HDFGRTR_EL2_PMSICR_EL1_MASK                     GENMASK(29, 29)
#define HDFGRTR_EL2_PMSICR_EL1_SHIFT                    29
#define HDFGRTR_EL2_PMSICR_EL1_WIDTH                    1

#define HDFGRTR_EL2_PMSFCR_EL1                          GENMASK(28, 28)
#define HDFGRTR_EL2_PMSFCR_EL1_MASK                     GENMASK(28, 28)
#define HDFGRTR_EL2_PMSFCR_EL1_SHIFT                    28
#define HDFGRTR_EL2_PMSFCR_EL1_WIDTH                    1

#define HDFGRTR_EL2_PMSEVFR_EL1                         GENMASK(27, 27)
#define HDFGRTR_EL2_PMSEVFR_EL1_MASK                    GENMASK(27, 27)
#define HDFGRTR_EL2_PMSEVFR_EL1_SHIFT                   27
#define HDFGRTR_EL2_PMSEVFR_EL1_WIDTH                   1

#define HDFGRTR_EL2_PMSCR_EL1                           GENMASK(26, 26)
#define HDFGRTR_EL2_PMSCR_EL1_MASK                      GENMASK(26, 26)
#define HDFGRTR_EL2_PMSCR_EL1_SHIFT                     26
#define HDFGRTR_EL2_PMSCR_EL1_WIDTH                     1

#define HDFGRTR_EL2_PMBSR_EL1                           GENMASK(25, 25)
#define HDFGRTR_EL2_PMBSR_EL1_MASK                      GENMASK(25, 25)
#define HDFGRTR_EL2_PMBSR_EL1_SHIFT                     25
#define HDFGRTR_EL2_PMBSR_EL1_WIDTH                     1

#define HDFGRTR_EL2_PMBPTR_EL1                          GENMASK(24, 24)
#define HDFGRTR_EL2_PMBPTR_EL1_MASK                     GENMASK(24, 24)
#define HDFGRTR_EL2_PMBPTR_EL1_SHIFT                    24
#define HDFGRTR_EL2_PMBPTR_EL1_WIDTH                    1

#define HDFGRTR_EL2_PMBLIMITR_EL1                       GENMASK(23, 23)
#define HDFGRTR_EL2_PMBLIMITR_EL1_MASK                  GENMASK(23, 23)
#define HDFGRTR_EL2_PMBLIMITR_EL1_SHIFT                 23
#define HDFGRTR_EL2_PMBLIMITR_EL1_WIDTH                 1

#define HDFGRTR_EL2_PMMIR_EL1                           GENMASK(22, 22)
#define HDFGRTR_EL2_PMMIR_EL1_MASK                      GENMASK(22, 22)
#define HDFGRTR_EL2_PMMIR_EL1_SHIFT                     22
#define HDFGRTR_EL2_PMMIR_EL1_WIDTH                     1

#define HDFGRTR_EL2_PMSELR_EL0                          GENMASK(19, 19)
#define HDFGRTR_EL2_PMSELR_EL0_MASK                     GENMASK(19, 19)
#define HDFGRTR_EL2_PMSELR_EL0_SHIFT                    19
#define HDFGRTR_EL2_PMSELR_EL0_WIDTH                    1

#define HDFGRTR_EL2_PMOVS                               GENMASK(18, 18)
#define HDFGRTR_EL2_PMOVS_MASK                          GENMASK(18, 18)
#define HDFGRTR_EL2_PMOVS_SHIFT                         18
#define HDFGRTR_EL2_PMOVS_WIDTH                         1

#define HDFGRTR_EL2_PMINTEN                             GENMASK(17, 17)
#define HDFGRTR_EL2_PMINTEN_MASK                        GENMASK(17, 17)
#define HDFGRTR_EL2_PMINTEN_SHIFT                       17
#define HDFGRTR_EL2_PMINTEN_WIDTH                       1

#define HDFGRTR_EL2_PMCNTEN                             GENMASK(16, 16)
#define HDFGRTR_EL2_PMCNTEN_MASK                        GENMASK(16, 16)
#define HDFGRTR_EL2_PMCNTEN_SHIFT                       16
#define HDFGRTR_EL2_PMCNTEN_WIDTH                       1

#define HDFGRTR_EL2_PMCCNTR_EL0                         GENMASK(15, 15)
#define HDFGRTR_EL2_PMCCNTR_EL0_MASK                    GENMASK(15, 15)
#define HDFGRTR_EL2_PMCCNTR_EL0_SHIFT                   15
#define HDFGRTR_EL2_PMCCNTR_EL0_WIDTH                   1

#define HDFGRTR_EL2_PMCCFILTR_EL0                       GENMASK(14, 14)
#define HDFGRTR_EL2_PMCCFILTR_EL0_MASK                  GENMASK(14, 14)
#define HDFGRTR_EL2_PMCCFILTR_EL0_SHIFT                 14
#define HDFGRTR_EL2_PMCCFILTR_EL0_WIDTH                 1

#define HDFGRTR_EL2_PMEVTYPERn_EL0                      GENMASK(13, 13)
#define HDFGRTR_EL2_PMEVTYPERn_EL0_MASK                 GENMASK(13, 13)
#define HDFGRTR_EL2_PMEVTYPERn_EL0_SHIFT                13
#define HDFGRTR_EL2_PMEVTYPERn_EL0_WIDTH                1

#define HDFGRTR_EL2_PMEVCNTRn_EL0                       GENMASK(12, 12)
#define HDFGRTR_EL2_PMEVCNTRn_EL0_MASK                  GENMASK(12, 12)
#define HDFGRTR_EL2_PMEVCNTRn_EL0_SHIFT                 12
#define HDFGRTR_EL2_PMEVCNTRn_EL0_WIDTH                 1

#define HDFGRTR_EL2_OSDLR_EL1                           GENMASK(11, 11)
#define HDFGRTR_EL2_OSDLR_EL1_MASK                      GENMASK(11, 11)
#define HDFGRTR_EL2_OSDLR_EL1_SHIFT                     11
#define HDFGRTR_EL2_OSDLR_EL1_WIDTH                     1

#define HDFGRTR_EL2_OSECCR_EL1                          GENMASK(10, 10)
#define HDFGRTR_EL2_OSECCR_EL1_MASK                     GENMASK(10, 10)
#define HDFGRTR_EL2_OSECCR_EL1_SHIFT                    10
#define HDFGRTR_EL2_OSECCR_EL1_WIDTH                    1

#define HDFGRTR_EL2_OSLSR_EL1                           GENMASK(9, 9)
#define HDFGRTR_EL2_OSLSR_EL1_MASK                      GENMASK(9, 9)
#define HDFGRTR_EL2_OSLSR_EL1_SHIFT                     9
#define HDFGRTR_EL2_OSLSR_EL1_WIDTH                     1

#define HDFGRTR_EL2_DBGPRCR_EL1                         GENMASK(7, 7)
#define HDFGRTR_EL2_DBGPRCR_EL1_MASK                    GENMASK(7, 7)
#define HDFGRTR_EL2_DBGPRCR_EL1_SHIFT                   7
#define HDFGRTR_EL2_DBGPRCR_EL1_WIDTH                   1

#define HDFGRTR_EL2_DBGAUTHSTATUS_EL1                   GENMASK(6, 6)
#define HDFGRTR_EL2_DBGAUTHSTATUS_EL1_MASK              GENMASK(6, 6)
#define HDFGRTR_EL2_DBGAUTHSTATUS_EL1_SHIFT             6
#define HDFGRTR_EL2_DBGAUTHSTATUS_EL1_WIDTH             1

#define HDFGRTR_EL2_DBGCLAIM                            GENMASK(5, 5)
#define HDFGRTR_EL2_DBGCLAIM_MASK                       GENMASK(5, 5)
#define HDFGRTR_EL2_DBGCLAIM_SHIFT                      5
#define HDFGRTR_EL2_DBGCLAIM_WIDTH                      1

#define HDFGRTR_EL2_MDSCR_EL1                           GENMASK(4, 4)
#define HDFGRTR_EL2_MDSCR_EL1_MASK                      GENMASK(4, 4)
#define HDFGRTR_EL2_MDSCR_EL1_SHIFT                     4
#define HDFGRTR_EL2_MDSCR_EL1_WIDTH                     1

#define HDFGRTR_EL2_DBGWVRn_EL1                         GENMASK(3, 3)
#define HDFGRTR_EL2_DBGWVRn_EL1_MASK                    GENMASK(3, 3)
#define HDFGRTR_EL2_DBGWVRn_EL1_SHIFT                   3
#define HDFGRTR_EL2_DBGWVRn_EL1_WIDTH                   1

#define HDFGRTR_EL2_DBGWCRn_EL1                         GENMASK(2, 2)
#define HDFGRTR_EL2_DBGWCRn_EL1_MASK                    GENMASK(2, 2)
#define HDFGRTR_EL2_DBGWCRn_EL1_SHIFT                   2
#define HDFGRTR_EL2_DBGWCRn_EL1_WIDTH                   1

#define HDFGRTR_EL2_DBGBVRn_EL1                         GENMASK(1, 1)
#define HDFGRTR_EL2_DBGBVRn_EL1_MASK                    GENMASK(1, 1)
#define HDFGRTR_EL2_DBGBVRn_EL1_SHIFT                   1
#define HDFGRTR_EL2_DBGBVRn_EL1_WIDTH                   1

#define HDFGRTR_EL2_DBGBCRn_EL1                         GENMASK(0, 0)
#define HDFGRTR_EL2_DBGBCRn_EL1_MASK                    GENMASK(0, 0)
#define HDFGRTR_EL2_DBGBCRn_EL1_SHIFT                   0
#define HDFGRTR_EL2_DBGBCRn_EL1_WIDTH                   1

#define HDFGRTR_EL2_RES0                                (UL(0) | GENMASK_ULL(49, 49) | GENMASK_ULL(42, 42) | GENMASK_ULL(39, 38) | GENMASK_ULL(21, 20) | GENMASK_ULL(8, 8))
#define HDFGRTR_EL2_RES1                                (UL(0))
#define HDFGRTR_EL2_UNKN                                (UL(0))

#define REG_HDFGWTR_EL2                                 S3_4_C3_C1_5
#define SYS_HDFGWTR_EL2                                 sys_reg(3, 4, 3, 1, 5)
#define SYS_HDFGWTR_EL2_Op0                             3
#define SYS_HDFGWTR_EL2_Op1                             4
#define SYS_HDFGWTR_EL2_CRn                             3
#define SYS_HDFGWTR_EL2_CRm                             1
#define SYS_HDFGWTR_EL2_Op2                             5

#define HDFGWTR_EL2_nPMSNEVFR_EL1                       GENMASK(62, 62)
#define HDFGWTR_EL2_nPMSNEVFR_EL1_MASK                  GENMASK(62, 62)
#define HDFGWTR_EL2_nPMSNEVFR_EL1_SHIFT                 62
#define HDFGWTR_EL2_nPMSNEVFR_EL1_WIDTH                 1

#define HDFGWTR_EL2_nBRBDATA                            GENMASK(61, 61)
#define HDFGWTR_EL2_nBRBDATA_MASK                       GENMASK(61, 61)
#define HDFGWTR_EL2_nBRBDATA_SHIFT                      61
#define HDFGWTR_EL2_nBRBDATA_WIDTH                      1

#define HDFGWTR_EL2_nBRBCTL                             GENMASK(60, 60)
#define HDFGWTR_EL2_nBRBCTL_MASK                        GENMASK(60, 60)
#define HDFGWTR_EL2_nBRBCTL_SHIFT                       60
#define HDFGWTR_EL2_nBRBCTL_WIDTH                       1

#define HDFGWTR_EL2_PMUSERENR_EL0                       GENMASK(57, 57)
#define HDFGWTR_EL2_PMUSERENR_EL0_MASK                  GENMASK(57, 57)
#define HDFGWTR_EL2_PMUSERENR_EL0_SHIFT                 57
#define HDFGWTR_EL2_PMUSERENR_EL0_WIDTH                 1

#define HDFGWTR_EL2_TRBTRG_EL1                          GENMASK(56, 56)
#define HDFGWTR_EL2_TRBTRG_EL1_MASK                     GENMASK(56, 56)
#define HDFGWTR_EL2_TRBTRG_EL1_SHIFT                    56
#define HDFGWTR_EL2_TRBTRG_EL1_WIDTH                    1

#define HDFGWTR_EL2_TRBSR_EL1                           GENMASK(55, 55)
#define HDFGWTR_EL2_TRBSR_EL1_MASK                      GENMASK(55, 55)
#define HDFGWTR_EL2_TRBSR_EL1_SHIFT                     55
#define HDFGWTR_EL2_TRBSR_EL1_WIDTH                     1

#define HDFGWTR_EL2_TRBPTR_EL1                          GENMASK(54, 54)
#define HDFGWTR_EL2_TRBPTR_EL1_MASK                     GENMASK(54, 54)
#define HDFGWTR_EL2_TRBPTR_EL1_SHIFT                    54
#define HDFGWTR_EL2_TRBPTR_EL1_WIDTH                    1

#define HDFGWTR_EL2_TRBMAR_EL1                          GENMASK(53, 53)
#define HDFGWTR_EL2_TRBMAR_EL1_MASK                     GENMASK(53, 53)
#define HDFGWTR_EL2_TRBMAR_EL1_SHIFT                    53
#define HDFGWTR_EL2_TRBMAR_EL1_WIDTH                    1

#define HDFGWTR_EL2_TRBLIMITR_EL1                       GENMASK(52, 52)
#define HDFGWTR_EL2_TRBLIMITR_EL1_MASK                  GENMASK(52, 52)
#define HDFGWTR_EL2_TRBLIMITR_EL1_SHIFT                 52
#define HDFGWTR_EL2_TRBLIMITR_EL1_WIDTH                 1

#define HDFGWTR_EL2_TRBBASER_EL1                        GENMASK(50, 50)
#define HDFGWTR_EL2_TRBBASER_EL1_MASK                   GENMASK(50, 50)
#define HDFGWTR_EL2_TRBBASER_EL1_SHIFT                  50
#define HDFGWTR_EL2_TRBBASER_EL1_WIDTH                  1

#define HDFGWTR_EL2_TRFCR_EL1                           GENMASK(49, 49)
#define HDFGWTR_EL2_TRFCR_EL1_MASK                      GENMASK(49, 49)
#define HDFGWTR_EL2_TRFCR_EL1_SHIFT                     49
#define HDFGWTR_EL2_TRFCR_EL1_WIDTH                     1

#define HDFGWTR_EL2_TRCVICTLR                           GENMASK(48, 48)
#define HDFGWTR_EL2_TRCVICTLR_MASK                      GENMASK(48, 48)
#define HDFGWTR_EL2_TRCVICTLR_SHIFT                     48
#define HDFGWTR_EL2_TRCVICTLR_WIDTH                     1

#define HDFGWTR_EL2_TRCSSCSRn                           GENMASK(46, 46)
#define HDFGWTR_EL2_TRCSSCSRn_MASK                      GENMASK(46, 46)
#define HDFGWTR_EL2_TRCSSCSRn_SHIFT                     46
#define HDFGWTR_EL2_TRCSSCSRn_WIDTH                     1

#define HDFGWTR_EL2_TRCSEQSTR                           GENMASK(45, 45)
#define HDFGWTR_EL2_TRCSEQSTR_MASK                      GENMASK(45, 45)
#define HDFGWTR_EL2_TRCSEQSTR_SHIFT                     45
#define HDFGWTR_EL2_TRCSEQSTR_WIDTH                     1

#define HDFGWTR_EL2_TRCPRGCTLR                          GENMASK(44, 44)
#define HDFGWTR_EL2_TRCPRGCTLR_MASK                     GENMASK(44, 44)
#define HDFGWTR_EL2_TRCPRGCTLR_SHIFT                    44
#define HDFGWTR_EL2_TRCPRGCTLR_WIDTH                    1

#define HDFGWTR_EL2_TRCOSLAR                            GENMASK(42, 42)
#define HDFGWTR_EL2_TRCOSLAR_MASK                       GENMASK(42, 42)
#define HDFGWTR_EL2_TRCOSLAR_SHIFT                      42
#define HDFGWTR_EL2_TRCOSLAR_WIDTH                      1

#define HDFGWTR_EL2_TRCIMSPECn                          GENMASK(41, 41)
#define HDFGWTR_EL2_TRCIMSPECn_MASK                     GENMASK(41, 41)
#define HDFGWTR_EL2_TRCIMSPECn_SHIFT                    41
#define HDFGWTR_EL2_TRCIMSPECn_WIDTH                    1

#define HDFGWTR_EL2_TRCCNTVRn                           GENMASK(37, 37)
#define HDFGWTR_EL2_TRCCNTVRn_MASK                      GENMASK(37, 37)
#define HDFGWTR_EL2_TRCCNTVRn_SHIFT                     37
#define HDFGWTR_EL2_TRCCNTVRn_WIDTH                     1

#define HDFGWTR_EL2_TRCCLAIM                            GENMASK(36, 36)
#define HDFGWTR_EL2_TRCCLAIM_MASK                       GENMASK(36, 36)
#define HDFGWTR_EL2_TRCCLAIM_SHIFT                      36
#define HDFGWTR_EL2_TRCCLAIM_WIDTH                      1

#define HDFGWTR_EL2_TRCAUXCTLR                          GENMASK(35, 35)
#define HDFGWTR_EL2_TRCAUXCTLR_MASK                     GENMASK(35, 35)
#define HDFGWTR_EL2_TRCAUXCTLR_SHIFT                    35
#define HDFGWTR_EL2_TRCAUXCTLR_WIDTH                    1

#define HDFGWTR_EL2_TRC                                 GENMASK(33, 33)
#define HDFGWTR_EL2_TRC_MASK                            GENMASK(33, 33)
#define HDFGWTR_EL2_TRC_SHIFT                           33
#define HDFGWTR_EL2_TRC_WIDTH                           1

#define HDFGWTR_EL2_PMSLATFR_EL1                        GENMASK(32, 32)
#define HDFGWTR_EL2_PMSLATFR_EL1_MASK                   GENMASK(32, 32)
#define HDFGWTR_EL2_PMSLATFR_EL1_SHIFT                  32
#define HDFGWTR_EL2_PMSLATFR_EL1_WIDTH                  1

#define HDFGWTR_EL2_PMSIRR_EL1                          GENMASK(31, 31)
#define HDFGWTR_EL2_PMSIRR_EL1_MASK                     GENMASK(31, 31)
#define HDFGWTR_EL2_PMSIRR_EL1_SHIFT                    31
#define HDFGWTR_EL2_PMSIRR_EL1_WIDTH                    1

#define HDFGWTR_EL2_PMSICR_EL1                          GENMASK(29, 29)
#define HDFGWTR_EL2_PMSICR_EL1_MASK                     GENMASK(29, 29)
#define HDFGWTR_EL2_PMSICR_EL1_SHIFT                    29
#define HDFGWTR_EL2_PMSICR_EL1_WIDTH                    1

#define HDFGWTR_EL2_PMSFCR_EL1                          GENMASK(28, 28)
#define HDFGWTR_EL2_PMSFCR_EL1_MASK                     GENMASK(28, 28)
#define HDFGWTR_EL2_PMSFCR_EL1_SHIFT                    28
#define HDFGWTR_EL2_PMSFCR_EL1_WIDTH                    1

#define HDFGWTR_EL2_PMSEVFR_EL1                         GENMASK(27, 27)
#define HDFGWTR_EL2_PMSEVFR_EL1_MASK                    GENMASK(27, 27)
#define HDFGWTR_EL2_PMSEVFR_EL1_SHIFT                   27
#define HDFGWTR_EL2_PMSEVFR_EL1_WIDTH                   1

#define HDFGWTR_EL2_PMSCR_EL1                           GENMASK(26, 26)
#define HDFGWTR_EL2_PMSCR_EL1_MASK                      GENMASK(26, 26)
#define HDFGWTR_EL2_PMSCR_EL1_SHIFT                     26
#define HDFGWTR_EL2_PMSCR_EL1_WIDTH                     1

#define HDFGWTR_EL2_PMBSR_EL1                           GENMASK(25, 25)
#define HDFGWTR_EL2_PMBSR_EL1_MASK                      GENMASK(25, 25)
#define HDFGWTR_EL2_PMBSR_EL1_SHIFT                     25
#define HDFGWTR_EL2_PMBSR_EL1_WIDTH                     1

#define HDFGWTR_EL2_PMBPTR_EL1                          GENMASK(24, 24)
#define HDFGWTR_EL2_PMBPTR_EL1_MASK                     GENMASK(24, 24)
#define HDFGWTR_EL2_PMBPTR_EL1_SHIFT                    24
#define HDFGWTR_EL2_PMBPTR_EL1_WIDTH                    1

#define HDFGWTR_EL2_PMBLIMITR_EL1                       GENMASK(23, 23)
#define HDFGWTR_EL2_PMBLIMITR_EL1_MASK                  GENMASK(23, 23)
#define HDFGWTR_EL2_PMBLIMITR_EL1_SHIFT                 23
#define HDFGWTR_EL2_PMBLIMITR_EL1_WIDTH                 1

#define HDFGWTR_EL2_PMCR_EL0                            GENMASK(21, 21)
#define HDFGWTR_EL2_PMCR_EL0_MASK                       GENMASK(21, 21)
#define HDFGWTR_EL2_PMCR_EL0_SHIFT                      21
#define HDFGWTR_EL2_PMCR_EL0_WIDTH                      1

#define HDFGWTR_EL2_PMSWINC_EL0                         GENMASK(20, 20)
#define HDFGWTR_EL2_PMSWINC_EL0_MASK                    GENMASK(20, 20)
#define HDFGWTR_EL2_PMSWINC_EL0_SHIFT                   20
#define HDFGWTR_EL2_PMSWINC_EL0_WIDTH                   1

#define HDFGWTR_EL2_PMSELR_EL0                          GENMASK(19, 19)
#define HDFGWTR_EL2_PMSELR_EL0_MASK                     GENMASK(19, 19)
#define HDFGWTR_EL2_PMSELR_EL0_SHIFT                    19
#define HDFGWTR_EL2_PMSELR_EL0_WIDTH                    1

#define HDFGWTR_EL2_PMOVS                               GENMASK(18, 18)
#define HDFGWTR_EL2_PMOVS_MASK                          GENMASK(18, 18)
#define HDFGWTR_EL2_PMOVS_SHIFT                         18
#define HDFGWTR_EL2_PMOVS_WIDTH                         1

#define HDFGWTR_EL2_PMINTEN                             GENMASK(17, 17)
#define HDFGWTR_EL2_PMINTEN_MASK                        GENMASK(17, 17)
#define HDFGWTR_EL2_PMINTEN_SHIFT                       17
#define HDFGWTR_EL2_PMINTEN_WIDTH                       1

#define HDFGWTR_EL2_PMCNTEN                             GENMASK(16, 16)
#define HDFGWTR_EL2_PMCNTEN_MASK                        GENMASK(16, 16)
#define HDFGWTR_EL2_PMCNTEN_SHIFT                       16
#define HDFGWTR_EL2_PMCNTEN_WIDTH                       1

#define HDFGWTR_EL2_PMCCNTR_EL0                         GENMASK(15, 15)
#define HDFGWTR_EL2_PMCCNTR_EL0_MASK                    GENMASK(15, 15)
#define HDFGWTR_EL2_PMCCNTR_EL0_SHIFT                   15
#define HDFGWTR_EL2_PMCCNTR_EL0_WIDTH                   1

#define HDFGWTR_EL2_PMCCFILTR_EL0                       GENMASK(14, 14)
#define HDFGWTR_EL2_PMCCFILTR_EL0_MASK                  GENMASK(14, 14)
#define HDFGWTR_EL2_PMCCFILTR_EL0_SHIFT                 14
#define HDFGWTR_EL2_PMCCFILTR_EL0_WIDTH                 1

#define HDFGWTR_EL2_PMEVTYPERn_EL0                      GENMASK(13, 13)
#define HDFGWTR_EL2_PMEVTYPERn_EL0_MASK                 GENMASK(13, 13)
#define HDFGWTR_EL2_PMEVTYPERn_EL0_SHIFT                13
#define HDFGWTR_EL2_PMEVTYPERn_EL0_WIDTH                1

#define HDFGWTR_EL2_PMEVCNTRn_EL0                       GENMASK(12, 12)
#define HDFGWTR_EL2_PMEVCNTRn_EL0_MASK                  GENMASK(12, 12)
#define HDFGWTR_EL2_PMEVCNTRn_EL0_SHIFT                 12
#define HDFGWTR_EL2_PMEVCNTRn_EL0_WIDTH                 1

#define HDFGWTR_EL2_OSDLR_EL1                           GENMASK(11, 11)
#define HDFGWTR_EL2_OSDLR_EL1_MASK                      GENMASK(11, 11)
#define HDFGWTR_EL2_OSDLR_EL1_SHIFT                     11
#define HDFGWTR_EL2_OSDLR_EL1_WIDTH                     1

#define HDFGWTR_EL2_OSECCR_EL1                          GENMASK(10, 10)
#define HDFGWTR_EL2_OSECCR_EL1_MASK                     GENMASK(10, 10)
#define HDFGWTR_EL2_OSECCR_EL1_SHIFT                    10
#define HDFGWTR_EL2_OSECCR_EL1_WIDTH                    1

#define HDFGWTR_EL2_OSLAR_EL1                           GENMASK(8, 8)
#define HDFGWTR_EL2_OSLAR_EL1_MASK                      GENMASK(8, 8)
#define HDFGWTR_EL2_OSLAR_EL1_SHIFT                     8
#define HDFGWTR_EL2_OSLAR_EL1_WIDTH                     1

#define HDFGWTR_EL2_DBGPRCR_EL1                         GENMASK(7, 7)
#define HDFGWTR_EL2_DBGPRCR_EL1_MASK                    GENMASK(7, 7)
#define HDFGWTR_EL2_DBGPRCR_EL1_SHIFT                   7
#define HDFGWTR_EL2_DBGPRCR_EL1_WIDTH                   1

#define HDFGWTR_EL2_DBGCLAIM                            GENMASK(5, 5)
#define HDFGWTR_EL2_DBGCLAIM_MASK                       GENMASK(5, 5)
#define HDFGWTR_EL2_DBGCLAIM_SHIFT                      5
#define HDFGWTR_EL2_DBGCLAIM_WIDTH                      1

#define HDFGWTR_EL2_MDSCR_EL1                           GENMASK(4, 4)
#define HDFGWTR_EL2_MDSCR_EL1_MASK                      GENMASK(4, 4)
#define HDFGWTR_EL2_MDSCR_EL1_SHIFT                     4
#define HDFGWTR_EL2_MDSCR_EL1_WIDTH                     1

#define HDFGWTR_EL2_DBGWVRn_EL1                         GENMASK(3, 3)
#define HDFGWTR_EL2_DBGWVRn_EL1_MASK                    GENMASK(3, 3)
#define HDFGWTR_EL2_DBGWVRn_EL1_SHIFT                   3
#define HDFGWTR_EL2_DBGWVRn_EL1_WIDTH                   1

#define HDFGWTR_EL2_DBGWCRn_EL1                         GENMASK(2, 2)
#define HDFGWTR_EL2_DBGWCRn_EL1_MASK                    GENMASK(2, 2)
#define HDFGWTR_EL2_DBGWCRn_EL1_SHIFT                   2
#define HDFGWTR_EL2_DBGWCRn_EL1_WIDTH                   1

#define HDFGWTR_EL2_DBGBVRn_EL1                         GENMASK(1, 1)
#define HDFGWTR_EL2_DBGBVRn_EL1_MASK                    GENMASK(1, 1)
#define HDFGWTR_EL2_DBGBVRn_EL1_SHIFT                   1
#define HDFGWTR_EL2_DBGBVRn_EL1_WIDTH                   1

#define HDFGWTR_EL2_DBGBCRn_EL1                         GENMASK(0, 0)
#define HDFGWTR_EL2_DBGBCRn_EL1_MASK                    GENMASK(0, 0)
#define HDFGWTR_EL2_DBGBCRn_EL1_SHIFT                   0
#define HDFGWTR_EL2_DBGBCRn_EL1_WIDTH                   1

#define HDFGWTR_EL2_RES0                                (UL(0) | GENMASK_ULL(63, 63) | GENMASK_ULL(59, 58) | GENMASK_ULL(51, 51) | GENMASK_ULL(47, 47) | GENMASK_ULL(43, 43) | GENMASK_ULL(40, 38) | GENMASK_ULL(34, 34) | GENMASK_ULL(30, 30) | GENMASK_ULL(22, 22) | GENMASK_ULL(9, 9) | GENMASK_ULL(6, 6))
#define HDFGWTR_EL2_RES1                                (UL(0))
#define HDFGWTR_EL2_UNKN                                (UL(0))

#define REG_ZCR_EL2                                     S3_4_C1_C2_0
#define SYS_ZCR_EL2                                     sys_reg(3, 4, 1, 2, 0)
#define SYS_ZCR_EL2_Op0                                 3
#define SYS_ZCR_EL2_Op1                                 4
#define SYS_ZCR_EL2_CRn                                 1
#define SYS_ZCR_EL2_CRm                                 2
#define SYS_ZCR_EL2_Op2                                 0

/* For ZCR_EL2 fields see ZCR_ELx */

#define REG_HCRX_EL2                                    S3_4_C1_C2_2
#define SYS_HCRX_EL2                                    sys_reg(3, 4, 1, 2, 2)
#define SYS_HCRX_EL2_Op0                                3
#define SYS_HCRX_EL2_Op1                                4
#define SYS_HCRX_EL2_CRn                                1
#define SYS_HCRX_EL2_CRm                                2
#define SYS_HCRX_EL2_Op2                                2

#define HCRX_EL2_GCSEn                                  GENMASK(22, 22)
#define HCRX_EL2_GCSEn_MASK                             GENMASK(22, 22)
#define HCRX_EL2_GCSEn_SHIFT                            22
#define HCRX_EL2_GCSEn_WIDTH                            1

#define HCRX_EL2_EnIDCP128                              GENMASK(21, 21)
#define HCRX_EL2_EnIDCP128_MASK                         GENMASK(21, 21)
#define HCRX_EL2_EnIDCP128_SHIFT                        21
#define HCRX_EL2_EnIDCP128_WIDTH                        1

#define HCRX_EL2_EnSDERR                                GENMASK(20, 20)
#define HCRX_EL2_EnSDERR_MASK                           GENMASK(20, 20)
#define HCRX_EL2_EnSDERR_SHIFT                          20
#define HCRX_EL2_EnSDERR_WIDTH                          1

#define HCRX_EL2_TMEA                                   GENMASK(19, 19)
#define HCRX_EL2_TMEA_MASK                              GENMASK(19, 19)
#define HCRX_EL2_TMEA_SHIFT                             19
#define HCRX_EL2_TMEA_WIDTH                             1

#define HCRX_EL2_EnSNERR                                GENMASK(18, 18)
#define HCRX_EL2_EnSNERR_MASK                           GENMASK(18, 18)
#define HCRX_EL2_EnSNERR_SHIFT                          18
#define HCRX_EL2_EnSNERR_WIDTH                          1

#define HCRX_EL2_D128En                                 GENMASK(17, 17)
#define HCRX_EL2_D128En_MASK                            GENMASK(17, 17)
#define HCRX_EL2_D128En_SHIFT                           17
#define HCRX_EL2_D128En_WIDTH                           1

#define HCRX_EL2_PTTWI                                  GENMASK(16, 16)
#define HCRX_EL2_PTTWI_MASK                             GENMASK(16, 16)
#define HCRX_EL2_PTTWI_SHIFT                            16
#define HCRX_EL2_PTTWI_WIDTH                            1

#define HCRX_EL2_SCTLR2En                               GENMASK(15, 15)
#define HCRX_EL2_SCTLR2En_MASK                          GENMASK(15, 15)
#define HCRX_EL2_SCTLR2En_SHIFT                         15
#define HCRX_EL2_SCTLR2En_WIDTH                         1

#define HCRX_EL2_TCR2En                                 GENMASK(14, 14)
#define HCRX_EL2_TCR2En_MASK                            GENMASK(14, 14)
#define HCRX_EL2_TCR2En_SHIFT                           14
#define HCRX_EL2_TCR2En_WIDTH                           1

#define HCRX_EL2_MSCEn                                  GENMASK(11, 11)
#define HCRX_EL2_MSCEn_MASK                             GENMASK(11, 11)
#define HCRX_EL2_MSCEn_SHIFT                            11
#define HCRX_EL2_MSCEn_WIDTH                            1

#define HCRX_EL2_MCE2                                   GENMASK(10, 10)
#define HCRX_EL2_MCE2_MASK                              GENMASK(10, 10)
#define HCRX_EL2_MCE2_SHIFT                             10
#define HCRX_EL2_MCE2_WIDTH                             1

#define HCRX_EL2_CMOW                                   GENMASK(9, 9)
#define HCRX_EL2_CMOW_MASK                              GENMASK(9, 9)
#define HCRX_EL2_CMOW_SHIFT                             9
#define HCRX_EL2_CMOW_WIDTH                             1

#define HCRX_EL2_VFNMI                                  GENMASK(8, 8)
#define HCRX_EL2_VFNMI_MASK                             GENMASK(8, 8)
#define HCRX_EL2_VFNMI_SHIFT                            8
#define HCRX_EL2_VFNMI_WIDTH                            1

#define HCRX_EL2_VINMI                                  GENMASK(7, 7)
#define HCRX_EL2_VINMI_MASK                             GENMASK(7, 7)
#define HCRX_EL2_VINMI_SHIFT                            7
#define HCRX_EL2_VINMI_WIDTH                            1

#define HCRX_EL2_TALLINT                                GENMASK(6, 6)
#define HCRX_EL2_TALLINT_MASK                           GENMASK(6, 6)
#define HCRX_EL2_TALLINT_SHIFT                          6
#define HCRX_EL2_TALLINT_WIDTH                          1

#define HCRX_EL2_SMPME                                  GENMASK(5, 5)
#define HCRX_EL2_SMPME_MASK                             GENMASK(5, 5)
#define HCRX_EL2_SMPME_SHIFT                            5
#define HCRX_EL2_SMPME_WIDTH                            1

#define HCRX_EL2_FGTnXS                                 GENMASK(4, 4)
#define HCRX_EL2_FGTnXS_MASK                            GENMASK(4, 4)
#define HCRX_EL2_FGTnXS_SHIFT                           4
#define HCRX_EL2_FGTnXS_WIDTH                           1

#define HCRX_EL2_FnXS                                   GENMASK(3, 3)
#define HCRX_EL2_FnXS_MASK                              GENMASK(3, 3)
#define HCRX_EL2_FnXS_SHIFT                             3
#define HCRX_EL2_FnXS_WIDTH                             1

#define HCRX_EL2_EnASR                                  GENMASK(2, 2)
#define HCRX_EL2_EnASR_MASK                             GENMASK(2, 2)
#define HCRX_EL2_EnASR_SHIFT                            2
#define HCRX_EL2_EnASR_WIDTH                            1

#define HCRX_EL2_EnALS                                  GENMASK(1, 1)
#define HCRX_EL2_EnALS_MASK                             GENMASK(1, 1)
#define HCRX_EL2_EnALS_SHIFT                            1
#define HCRX_EL2_EnALS_WIDTH                            1

#define HCRX_EL2_EnAS0                                  GENMASK(0, 0)
#define HCRX_EL2_EnAS0_MASK                             GENMASK(0, 0)
#define HCRX_EL2_EnAS0_SHIFT                            0
#define HCRX_EL2_EnAS0_WIDTH                            1

#define HCRX_EL2_RES0                                   (UL(0) | GENMASK_ULL(63, 23) | GENMASK_ULL(13, 12))
#define HCRX_EL2_RES1                                   (UL(0))
#define HCRX_EL2_UNKN                                   (UL(0))

#define REG_SMPRIMAP_EL2                                S3_4_C1_C2_5
#define SYS_SMPRIMAP_EL2                                sys_reg(3, 4, 1, 2, 5)
#define SYS_SMPRIMAP_EL2_Op0                            3
#define SYS_SMPRIMAP_EL2_Op1                            4
#define SYS_SMPRIMAP_EL2_CRn                            1
#define SYS_SMPRIMAP_EL2_CRm                            2
#define SYS_SMPRIMAP_EL2_Op2                            5

#define SMPRIMAP_EL2_P15                                GENMASK(63, 60)
#define SMPRIMAP_EL2_P15_MASK                           GENMASK(63, 60)
#define SMPRIMAP_EL2_P15_SHIFT                          60
#define SMPRIMAP_EL2_P15_WIDTH                          4

#define SMPRIMAP_EL2_P14                                GENMASK(59, 56)
#define SMPRIMAP_EL2_P14_MASK                           GENMASK(59, 56)
#define SMPRIMAP_EL2_P14_SHIFT                          56
#define SMPRIMAP_EL2_P14_WIDTH                          4

#define SMPRIMAP_EL2_P13                                GENMASK(55, 52)
#define SMPRIMAP_EL2_P13_MASK                           GENMASK(55, 52)
#define SMPRIMAP_EL2_P13_SHIFT                          52
#define SMPRIMAP_EL2_P13_WIDTH                          4

#define SMPRIMAP_EL2_P12                                GENMASK(51, 48)
#define SMPRIMAP_EL2_P12_MASK                           GENMASK(51, 48)
#define SMPRIMAP_EL2_P12_SHIFT                          48
#define SMPRIMAP_EL2_P12_WIDTH                          4

#define SMPRIMAP_EL2_P11                                GENMASK(47, 44)
#define SMPRIMAP_EL2_P11_MASK                           GENMASK(47, 44)
#define SMPRIMAP_EL2_P11_SHIFT                          44
#define SMPRIMAP_EL2_P11_WIDTH                          4

#define SMPRIMAP_EL2_P10                                GENMASK(43, 40)
#define SMPRIMAP_EL2_P10_MASK                           GENMASK(43, 40)
#define SMPRIMAP_EL2_P10_SHIFT                          40
#define SMPRIMAP_EL2_P10_WIDTH                          4

#define SMPRIMAP_EL2_F9                                 GENMASK(39, 36)
#define SMPRIMAP_EL2_F9_MASK                            GENMASK(39, 36)
#define SMPRIMAP_EL2_F9_SHIFT                           36
#define SMPRIMAP_EL2_F9_WIDTH                           4

#define SMPRIMAP_EL2_P8                                 GENMASK(35, 32)
#define SMPRIMAP_EL2_P8_MASK                            GENMASK(35, 32)
#define SMPRIMAP_EL2_P8_SHIFT                           32
#define SMPRIMAP_EL2_P8_WIDTH                           4

#define SMPRIMAP_EL2_P7                                 GENMASK(31, 28)
#define SMPRIMAP_EL2_P7_MASK                            GENMASK(31, 28)
#define SMPRIMAP_EL2_P7_SHIFT                           28
#define SMPRIMAP_EL2_P7_WIDTH                           4

#define SMPRIMAP_EL2_P6                                 GENMASK(27, 24)
#define SMPRIMAP_EL2_P6_MASK                            GENMASK(27, 24)
#define SMPRIMAP_EL2_P6_SHIFT                           24
#define SMPRIMAP_EL2_P6_WIDTH                           4

#define SMPRIMAP_EL2_P5                                 GENMASK(23, 20)
#define SMPRIMAP_EL2_P5_MASK                            GENMASK(23, 20)
#define SMPRIMAP_EL2_P5_SHIFT                           20
#define SMPRIMAP_EL2_P5_WIDTH                           4

#define SMPRIMAP_EL2_P4                                 GENMASK(19, 16)
#define SMPRIMAP_EL2_P4_MASK                            GENMASK(19, 16)
#define SMPRIMAP_EL2_P4_SHIFT                           16
#define SMPRIMAP_EL2_P4_WIDTH                           4

#define SMPRIMAP_EL2_P3                                 GENMASK(15, 12)
#define SMPRIMAP_EL2_P3_MASK                            GENMASK(15, 12)
#define SMPRIMAP_EL2_P3_SHIFT                           12
#define SMPRIMAP_EL2_P3_WIDTH                           4

#define SMPRIMAP_EL2_P2                                 GENMASK(11, 8)
#define SMPRIMAP_EL2_P2_MASK                            GENMASK(11, 8)
#define SMPRIMAP_EL2_P2_SHIFT                           8
#define SMPRIMAP_EL2_P2_WIDTH                           4

#define SMPRIMAP_EL2_P1                                 GENMASK(7, 4)
#define SMPRIMAP_EL2_P1_MASK                            GENMASK(7, 4)
#define SMPRIMAP_EL2_P1_SHIFT                           4
#define SMPRIMAP_EL2_P1_WIDTH                           4

#define SMPRIMAP_EL2_P0                                 GENMASK(3, 0)
#define SMPRIMAP_EL2_P0_MASK                            GENMASK(3, 0)
#define SMPRIMAP_EL2_P0_SHIFT                           0
#define SMPRIMAP_EL2_P0_WIDTH                           4

#define SMPRIMAP_EL2_RES0                               (UL(0))
#define SMPRIMAP_EL2_RES1                               (UL(0))
#define SMPRIMAP_EL2_UNKN                               (UL(0))

#define REG_SMCR_EL2                                    S3_4_C1_C2_6
#define SYS_SMCR_EL2                                    sys_reg(3, 4, 1, 2, 6)
#define SYS_SMCR_EL2_Op0                                3
#define SYS_SMCR_EL2_Op1                                4
#define SYS_SMCR_EL2_CRn                                1
#define SYS_SMCR_EL2_CRm                                2
#define SYS_SMCR_EL2_Op2                                6

/* For SMCR_EL2 fields see SMCR_ELx */

#define REG_DACR32_EL2                                  S3_4_C3_C0_0
#define SYS_DACR32_EL2                                  sys_reg(3, 4, 3, 0, 0)
#define SYS_DACR32_EL2_Op0                              3
#define SYS_DACR32_EL2_Op1                              4
#define SYS_DACR32_EL2_CRn                              3
#define SYS_DACR32_EL2_CRm                              0
#define SYS_DACR32_EL2_Op2                              0

#define DACR32_EL2_D15                                  GENMASK(31, 30)
#define DACR32_EL2_D15_MASK                             GENMASK(31, 30)
#define DACR32_EL2_D15_SHIFT                            30
#define DACR32_EL2_D15_WIDTH                            2

#define DACR32_EL2_D14                                  GENMASK(29, 28)
#define DACR32_EL2_D14_MASK                             GENMASK(29, 28)
#define DACR32_EL2_D14_SHIFT                            28
#define DACR32_EL2_D14_WIDTH                            2

#define DACR32_EL2_D13                                  GENMASK(27, 26)
#define DACR32_EL2_D13_MASK                             GENMASK(27, 26)
#define DACR32_EL2_D13_SHIFT                            26
#define DACR32_EL2_D13_WIDTH                            2

#define DACR32_EL2_D12                                  GENMASK(25, 24)
#define DACR32_EL2_D12_MASK                             GENMASK(25, 24)
#define DACR32_EL2_D12_SHIFT                            24
#define DACR32_EL2_D12_WIDTH                            2

#define DACR32_EL2_D11                                  GENMASK(23, 22)
#define DACR32_EL2_D11_MASK                             GENMASK(23, 22)
#define DACR32_EL2_D11_SHIFT                            22
#define DACR32_EL2_D11_WIDTH                            2

#define DACR32_EL2_D10                                  GENMASK(21, 20)
#define DACR32_EL2_D10_MASK                             GENMASK(21, 20)
#define DACR32_EL2_D10_SHIFT                            20
#define DACR32_EL2_D10_WIDTH                            2

#define DACR32_EL2_D9                                   GENMASK(19, 18)
#define DACR32_EL2_D9_MASK                              GENMASK(19, 18)
#define DACR32_EL2_D9_SHIFT                             18
#define DACR32_EL2_D9_WIDTH                             2

#define DACR32_EL2_D8                                   GENMASK(17, 16)
#define DACR32_EL2_D8_MASK                              GENMASK(17, 16)
#define DACR32_EL2_D8_SHIFT                             16
#define DACR32_EL2_D8_WIDTH                             2

#define DACR32_EL2_D7                                   GENMASK(15, 14)
#define DACR32_EL2_D7_MASK                              GENMASK(15, 14)
#define DACR32_EL2_D7_SHIFT                             14
#define DACR32_EL2_D7_WIDTH                             2

#define DACR32_EL2_D6                                   GENMASK(13, 12)
#define DACR32_EL2_D6_MASK                              GENMASK(13, 12)
#define DACR32_EL2_D6_SHIFT                             12
#define DACR32_EL2_D6_WIDTH                             2

#define DACR32_EL2_D5                                   GENMASK(11, 10)
#define DACR32_EL2_D5_MASK                              GENMASK(11, 10)
#define DACR32_EL2_D5_SHIFT                             10
#define DACR32_EL2_D5_WIDTH                             2

#define DACR32_EL2_D4                                   GENMASK(9, 8)
#define DACR32_EL2_D4_MASK                              GENMASK(9, 8)
#define DACR32_EL2_D4_SHIFT                             8
#define DACR32_EL2_D4_WIDTH                             2

#define DACR32_EL2_D3                                   GENMASK(7, 6)
#define DACR32_EL2_D3_MASK                              GENMASK(7, 6)
#define DACR32_EL2_D3_SHIFT                             6
#define DACR32_EL2_D3_WIDTH                             2

#define DACR32_EL2_D2                                   GENMASK(5, 4)
#define DACR32_EL2_D2_MASK                              GENMASK(5, 4)
#define DACR32_EL2_D2_SHIFT                             4
#define DACR32_EL2_D2_WIDTH                             2

#define DACR32_EL2_D1                                   GENMASK(3, 2)
#define DACR32_EL2_D1_MASK                              GENMASK(3, 2)
#define DACR32_EL2_D1_SHIFT                             2
#define DACR32_EL2_D1_WIDTH                             2

#define DACR32_EL2_D0                                   GENMASK(1, 0)
#define DACR32_EL2_D0_MASK                              GENMASK(1, 0)
#define DACR32_EL2_D0_SHIFT                             0
#define DACR32_EL2_D0_WIDTH                             2

#define DACR32_EL2_RES0                                 (UL(0) | GENMASK_ULL(63, 32))
#define DACR32_EL2_RES1                                 (UL(0))
#define DACR32_EL2_UNKN                                 (UL(0))

#define REG_FAR_EL2                                     S3_4_C6_C0_0
#define SYS_FAR_EL2                                     sys_reg(3, 4, 6, 0, 0)
#define SYS_FAR_EL2_Op0                                 3
#define SYS_FAR_EL2_Op1                                 4
#define SYS_FAR_EL2_CRn                                 6
#define SYS_FAR_EL2_CRm                                 0
#define SYS_FAR_EL2_Op2                                 0

#define FAR_EL2_ADDR                                    GENMASK(63, 0)
#define FAR_EL2_ADDR_MASK                               GENMASK(63, 0)
#define FAR_EL2_ADDR_SHIFT                              0
#define FAR_EL2_ADDR_WIDTH                              64

#define FAR_EL2_RES0                                    (UL(0))
#define FAR_EL2_RES1                                    (UL(0))
#define FAR_EL2_UNKN                                    (UL(0))

#define REG_PMSCR_EL2                                   S3_4_C9_C9_0
#define SYS_PMSCR_EL2                                   sys_reg(3, 4, 9, 9, 0)
#define SYS_PMSCR_EL2_Op0                               3
#define SYS_PMSCR_EL2_Op1                               4
#define SYS_PMSCR_EL2_CRn                               9
#define SYS_PMSCR_EL2_CRm                               9
#define SYS_PMSCR_EL2_Op2                               0

#define PMSCR_EL2_PCT                                   GENMASK(7, 6)
#define PMSCR_EL2_PCT_MASK                              GENMASK(7, 6)
#define PMSCR_EL2_PCT_SHIFT                             6
#define PMSCR_EL2_PCT_WIDTH                             2
#define PMSCR_EL2_PCT_VIRT                              UL(0b00)
#define PMSCR_EL2_PCT_PHYS                              UL(0b01)
#define PMSCR_EL2_PCT_GUEST                             UL(0b11)

#define PMSCR_EL2_TS                                    GENMASK(5, 5)
#define PMSCR_EL2_TS_MASK                               GENMASK(5, 5)
#define PMSCR_EL2_TS_SHIFT                              5
#define PMSCR_EL2_TS_WIDTH                              1

#define PMSCR_EL2_PA                                    GENMASK(4, 4)
#define PMSCR_EL2_PA_MASK                               GENMASK(4, 4)
#define PMSCR_EL2_PA_SHIFT                              4
#define PMSCR_EL2_PA_WIDTH                              1

#define PMSCR_EL2_CX                                    GENMASK(3, 3)
#define PMSCR_EL2_CX_MASK                               GENMASK(3, 3)
#define PMSCR_EL2_CX_SHIFT                              3
#define PMSCR_EL2_CX_WIDTH                              1

#define PMSCR_EL2_E2SPE                                 GENMASK(1, 1)
#define PMSCR_EL2_E2SPE_MASK                            GENMASK(1, 1)
#define PMSCR_EL2_E2SPE_SHIFT                           1
#define PMSCR_EL2_E2SPE_WIDTH                           1

#define PMSCR_EL2_E0HSPE                                GENMASK(0, 0)
#define PMSCR_EL2_E0HSPE_MASK                           GENMASK(0, 0)
#define PMSCR_EL2_E0HSPE_SHIFT                          0
#define PMSCR_EL2_E0HSPE_WIDTH                          1

#define PMSCR_EL2_RES0                                  (UL(0) | GENMASK_ULL(63, 8) | GENMASK_ULL(2, 2))
#define PMSCR_EL2_RES1                                  (UL(0))
#define PMSCR_EL2_UNKN                                  (UL(0))

#define REG_CONTEXTIDR_EL2                              S3_4_C13_C0_1
#define SYS_CONTEXTIDR_EL2                              sys_reg(3, 4, 13, 0, 1)
#define SYS_CONTEXTIDR_EL2_Op0                          3
#define SYS_CONTEXTIDR_EL2_Op1                          4
#define SYS_CONTEXTIDR_EL2_CRn                          13
#define SYS_CONTEXTIDR_EL2_CRm                          0
#define SYS_CONTEXTIDR_EL2_Op2                          1

/* For CONTEXTIDR_EL2 fields see CONTEXTIDR_ELx */

#define REG_CNTPOFF_EL2                                 S3_4_C14_C0_6
#define SYS_CNTPOFF_EL2                                 sys_reg(3, 4, 14, 0, 6)
#define SYS_CNTPOFF_EL2_Op0                             3
#define SYS_CNTPOFF_EL2_Op1                             4
#define SYS_CNTPOFF_EL2_CRn                             14
#define SYS_CNTPOFF_EL2_CRm                             0
#define SYS_CNTPOFF_EL2_Op2                             6

#define CNTPOFF_EL2_PhysicalOffset                      GENMASK(63, 0)
#define CNTPOFF_EL2_PhysicalOffset_MASK                 GENMASK(63, 0)
#define CNTPOFF_EL2_PhysicalOffset_SHIFT                0
#define CNTPOFF_EL2_PhysicalOffset_WIDTH                64

#define CNTPOFF_EL2_RES0                                (UL(0))
#define CNTPOFF_EL2_RES1                                (UL(0))
#define CNTPOFF_EL2_UNKN                                (UL(0))

#define REG_CPACR_EL12                                  S3_5_C1_C0_2
#define SYS_CPACR_EL12                                  sys_reg(3, 5, 1, 0, 2)
#define SYS_CPACR_EL12_Op0                              3
#define SYS_CPACR_EL12_Op1                              5
#define SYS_CPACR_EL12_CRn                              1
#define SYS_CPACR_EL12_CRm                              0
#define SYS_CPACR_EL12_Op2                              2

/* For CPACR_EL12 fields see CPACR_ELx */

#define REG_ZCR_EL12                                    S3_5_C1_C2_0
#define SYS_ZCR_EL12                                    sys_reg(3, 5, 1, 2, 0)
#define SYS_ZCR_EL12_Op0                                3
#define SYS_ZCR_EL12_Op1                                5
#define SYS_ZCR_EL12_CRn                                1
#define SYS_ZCR_EL12_CRm                                2
#define SYS_ZCR_EL12_Op2                                0

/* For ZCR_EL12 fields see ZCR_ELx */

#define REG_SMCR_EL12                                   S3_5_C1_C2_6
#define SYS_SMCR_EL12                                   sys_reg(3, 5, 1, 2, 6)
#define SYS_SMCR_EL12_Op0                               3
#define SYS_SMCR_EL12_Op1                               5
#define SYS_SMCR_EL12_CRn                               1
#define SYS_SMCR_EL12_CRm                               2
#define SYS_SMCR_EL12_Op2                               6

/* For SMCR_EL12 fields see SMCR_ELx */

#define REG_FAR_EL12                                    S3_5_C6_C0_0
#define SYS_FAR_EL12                                    sys_reg(3, 5, 6, 0, 0)
#define SYS_FAR_EL12_Op0                                3
#define SYS_FAR_EL12_Op1                                5
#define SYS_FAR_EL12_CRn                                6
#define SYS_FAR_EL12_CRm                                0
#define SYS_FAR_EL12_Op2                                0

#define FAR_EL12_ADDR                                   GENMASK(63, 0)
#define FAR_EL12_ADDR_MASK                              GENMASK(63, 0)
#define FAR_EL12_ADDR_SHIFT                             0
#define FAR_EL12_ADDR_WIDTH                             64

#define FAR_EL12_RES0                                   (UL(0))
#define FAR_EL12_RES1                                   (UL(0))
#define FAR_EL12_UNKN                                   (UL(0))

#define REG_CONTEXTIDR_EL12                             S3_5_C13_C0_1
#define SYS_CONTEXTIDR_EL12                             sys_reg(3, 5, 13, 0, 1)
#define SYS_CONTEXTIDR_EL12_Op0                         3
#define SYS_CONTEXTIDR_EL12_Op1                         5
#define SYS_CONTEXTIDR_EL12_CRn                         13
#define SYS_CONTEXTIDR_EL12_CRm                         0
#define SYS_CONTEXTIDR_EL12_Op2                         1

/* For CONTEXTIDR_EL12 fields see CONTEXTIDR_ELx */

#define TTBRx_EL1_ASID                                  GENMASK(63, 48)
#define TTBRx_EL1_ASID_MASK                             GENMASK(63, 48)
#define TTBRx_EL1_ASID_SHIFT                            48
#define TTBRx_EL1_ASID_WIDTH                            16

#define TTBRx_EL1_BADDR                                 GENMASK(47, 1)
#define TTBRx_EL1_BADDR_MASK                            GENMASK(47, 1)
#define TTBRx_EL1_BADDR_SHIFT                           1
#define TTBRx_EL1_BADDR_WIDTH                           47

#define TTBRx_EL1_CnP                                   GENMASK(0, 0)
#define TTBRx_EL1_CnP_MASK                              GENMASK(0, 0)
#define TTBRx_EL1_CnP_SHIFT                             0
#define TTBRx_EL1_CnP_WIDTH                             1

#define TTBRx_EL1_RES0                                  (UL(0))
#define TTBRx_EL1_RES1                                  (UL(0))
#define TTBRx_EL1_UNKN                                  (UL(0))

#define REG_TTBR0_EL1                                   S3_0_C2_C0_0
#define SYS_TTBR0_EL1                                   sys_reg(3, 0, 2, 0, 0)
#define SYS_TTBR0_EL1_Op0                               3
#define SYS_TTBR0_EL1_Op1                               0
#define SYS_TTBR0_EL1_CRn                               2
#define SYS_TTBR0_EL1_CRm                               0
#define SYS_TTBR0_EL1_Op2                               0

/* For TTBR0_EL1 fields see TTBRx_EL1 */

#define REG_TTBR1_EL1                                   S3_0_C2_C0_1
#define SYS_TTBR1_EL1                                   sys_reg(3, 0, 2, 0, 1)
#define SYS_TTBR1_EL1_Op0                               3
#define SYS_TTBR1_EL1_Op1                               0
#define SYS_TTBR1_EL1_CRn                               2
#define SYS_TTBR1_EL1_CRm                               0
#define SYS_TTBR1_EL1_Op2                               1

/* For TTBR1_EL1 fields see TTBRx_EL1 */

#define TCR2_EL1x_DisCH1                                GENMASK(15, 15)
#define TCR2_EL1x_DisCH1_MASK                           GENMASK(15, 15)
#define TCR2_EL1x_DisCH1_SHIFT                          15
#define TCR2_EL1x_DisCH1_WIDTH                          1

#define TCR2_EL1x_DisCH0                                GENMASK(14, 14)
#define TCR2_EL1x_DisCH0_MASK                           GENMASK(14, 14)
#define TCR2_EL1x_DisCH0_SHIFT                          14
#define TCR2_EL1x_DisCH0_WIDTH                          1

#define TCR2_EL1x_HAFT                                  GENMASK(11, 11)
#define TCR2_EL1x_HAFT_MASK                             GENMASK(11, 11)
#define TCR2_EL1x_HAFT_SHIFT                            11
#define TCR2_EL1x_HAFT_WIDTH                            1

#define TCR2_EL1x_PTTWI                                 GENMASK(10, 10)
#define TCR2_EL1x_PTTWI_MASK                            GENMASK(10, 10)
#define TCR2_EL1x_PTTWI_SHIFT                           10
#define TCR2_EL1x_PTTWI_WIDTH                           1

#define TCR2_EL1x_D128                                  GENMASK(5, 5)
#define TCR2_EL1x_D128_MASK                             GENMASK(5, 5)
#define TCR2_EL1x_D128_SHIFT                            5
#define TCR2_EL1x_D128_WIDTH                            1

#define TCR2_EL1x_AIE                                   GENMASK(4, 4)
#define TCR2_EL1x_AIE_MASK                              GENMASK(4, 4)
#define TCR2_EL1x_AIE_SHIFT                             4
#define TCR2_EL1x_AIE_WIDTH                             1

#define TCR2_EL1x_POE                                   GENMASK(3, 3)
#define TCR2_EL1x_POE_MASK                              GENMASK(3, 3)
#define TCR2_EL1x_POE_SHIFT                             3
#define TCR2_EL1x_POE_WIDTH                             1

#define TCR2_EL1x_E0POE                                 GENMASK(2, 2)
#define TCR2_EL1x_E0POE_MASK                            GENMASK(2, 2)
#define TCR2_EL1x_E0POE_SHIFT                           2
#define TCR2_EL1x_E0POE_WIDTH                           1

#define TCR2_EL1x_PIE                                   GENMASK(1, 1)
#define TCR2_EL1x_PIE_MASK                              GENMASK(1, 1)
#define TCR2_EL1x_PIE_SHIFT                             1
#define TCR2_EL1x_PIE_WIDTH                             1

#define TCR2_EL1x_PnCH                                  GENMASK(0, 0)
#define TCR2_EL1x_PnCH_MASK                             GENMASK(0, 0)
#define TCR2_EL1x_PnCH_SHIFT                            0
#define TCR2_EL1x_PnCH_WIDTH                            1

#define TCR2_EL1x_RES0                                  (UL(0) | GENMASK_ULL(63, 16) | GENMASK_ULL(13, 12) | GENMASK_ULL(9, 6))
#define TCR2_EL1x_RES1                                  (UL(0))
#define TCR2_EL1x_UNKN                                  (UL(0))

#define REG_TCR2_EL1                                    S3_0_C2_C0_3
#define SYS_TCR2_EL1                                    sys_reg(3, 0, 2, 0, 3)
#define SYS_TCR2_EL1_Op0                                3
#define SYS_TCR2_EL1_Op1                                0
#define SYS_TCR2_EL1_CRn                                2
#define SYS_TCR2_EL1_CRm                                0
#define SYS_TCR2_EL1_Op2                                3

/* For TCR2_EL1 fields see TCR2_EL1x */

#define REG_TCR2_EL12                                   S3_5_C2_C0_3
#define SYS_TCR2_EL12                                   sys_reg(3, 5, 2, 0, 3)
#define SYS_TCR2_EL12_Op0                               3
#define SYS_TCR2_EL12_Op1                               5
#define SYS_TCR2_EL12_CRn                               2
#define SYS_TCR2_EL12_CRm                               0
#define SYS_TCR2_EL12_Op2                               3

/* For TCR2_EL12 fields see TCR2_EL1x */

#define REG_TCR2_EL2                                    S3_4_C2_C0_3
#define SYS_TCR2_EL2                                    sys_reg(3, 4, 2, 0, 3)
#define SYS_TCR2_EL2_Op0                                3
#define SYS_TCR2_EL2_Op1                                4
#define SYS_TCR2_EL2_CRn                                2
#define SYS_TCR2_EL2_CRm                                0
#define SYS_TCR2_EL2_Op2                                3

#define TCR2_EL2_DisCH1                                 GENMASK(15, 15)
#define TCR2_EL2_DisCH1_MASK                            GENMASK(15, 15)
#define TCR2_EL2_DisCH1_SHIFT                           15
#define TCR2_EL2_DisCH1_WIDTH                           1

#define TCR2_EL2_DisCH0                                 GENMASK(14, 14)
#define TCR2_EL2_DisCH0_MASK                            GENMASK(14, 14)
#define TCR2_EL2_DisCH0_SHIFT                           14
#define TCR2_EL2_DisCH0_WIDTH                           1

#define TCR2_EL2_AMEC1                                  GENMASK(13, 13)
#define TCR2_EL2_AMEC1_MASK                             GENMASK(13, 13)
#define TCR2_EL2_AMEC1_SHIFT                            13
#define TCR2_EL2_AMEC1_WIDTH                            1

#define TCR2_EL2_AMEC0                                  GENMASK(12, 12)
#define TCR2_EL2_AMEC0_MASK                             GENMASK(12, 12)
#define TCR2_EL2_AMEC0_SHIFT                            12
#define TCR2_EL2_AMEC0_WIDTH                            1

#define TCR2_EL2_HAFT                                   GENMASK(11, 11)
#define TCR2_EL2_HAFT_MASK                              GENMASK(11, 11)
#define TCR2_EL2_HAFT_SHIFT                             11
#define TCR2_EL2_HAFT_WIDTH                             1

#define TCR2_EL2_PTTWI                                  GENMASK(10, 10)
#define TCR2_EL2_PTTWI_MASK                             GENMASK(10, 10)
#define TCR2_EL2_PTTWI_SHIFT                            10
#define TCR2_EL2_PTTWI_WIDTH                            1

#define TCR2_EL2_SKL1                                   GENMASK(9, 8)
#define TCR2_EL2_SKL1_MASK                              GENMASK(9, 8)
#define TCR2_EL2_SKL1_SHIFT                             8
#define TCR2_EL2_SKL1_WIDTH                             2

#define TCR2_EL2_SKL0                                   GENMASK(7, 6)
#define TCR2_EL2_SKL0_MASK                              GENMASK(7, 6)
#define TCR2_EL2_SKL0_SHIFT                             6
#define TCR2_EL2_SKL0_WIDTH                             2

#define TCR2_EL2_D128                                   GENMASK(5, 5)
#define TCR2_EL2_D128_MASK                              GENMASK(5, 5)
#define TCR2_EL2_D128_SHIFT                             5
#define TCR2_EL2_D128_WIDTH                             1

#define TCR2_EL2_AIE                                    GENMASK(4, 4)
#define TCR2_EL2_AIE_MASK                               GENMASK(4, 4)
#define TCR2_EL2_AIE_SHIFT                              4
#define TCR2_EL2_AIE_WIDTH                              1

#define TCR2_EL2_POE                                    GENMASK(3, 3)
#define TCR2_EL2_POE_MASK                               GENMASK(3, 3)
#define TCR2_EL2_POE_SHIFT                              3
#define TCR2_EL2_POE_WIDTH                              1

#define TCR2_EL2_E0POE                                  GENMASK(2, 2)
#define TCR2_EL2_E0POE_MASK                             GENMASK(2, 2)
#define TCR2_EL2_E0POE_SHIFT                            2
#define TCR2_EL2_E0POE_WIDTH                            1

#define TCR2_EL2_PIE                                    GENMASK(1, 1)
#define TCR2_EL2_PIE_MASK                               GENMASK(1, 1)
#define TCR2_EL2_PIE_SHIFT                              1
#define TCR2_EL2_PIE_WIDTH                              1

#define TCR2_EL2_PnCH                                   GENMASK(0, 0)
#define TCR2_EL2_PnCH_MASK                              GENMASK(0, 0)
#define TCR2_EL2_PnCH_SHIFT                             0
#define TCR2_EL2_PnCH_WIDTH                             1

#define TCR2_EL2_RES0                                   (UL(0) | GENMASK_ULL(63, 16))
#define TCR2_EL2_RES1                                   (UL(0))
#define TCR2_EL2_UNKN                                   (UL(0))

#define PIRx_ELx_Perm15                                 GENMASK(63, 60)
#define PIRx_ELx_Perm15_MASK                            GENMASK(63, 60)
#define PIRx_ELx_Perm15_SHIFT                           60
#define PIRx_ELx_Perm15_WIDTH                           4

#define PIRx_ELx_Perm14                                 GENMASK(59, 56)
#define PIRx_ELx_Perm14_MASK                            GENMASK(59, 56)
#define PIRx_ELx_Perm14_SHIFT                           56
#define PIRx_ELx_Perm14_WIDTH                           4

#define PIRx_ELx_Perm13                                 GENMASK(55, 52)
#define PIRx_ELx_Perm13_MASK                            GENMASK(55, 52)
#define PIRx_ELx_Perm13_SHIFT                           52
#define PIRx_ELx_Perm13_WIDTH                           4

#define PIRx_ELx_Perm12                                 GENMASK(51, 48)
#define PIRx_ELx_Perm12_MASK                            GENMASK(51, 48)
#define PIRx_ELx_Perm12_SHIFT                           48
#define PIRx_ELx_Perm12_WIDTH                           4

#define PIRx_ELx_Perm11                                 GENMASK(47, 44)
#define PIRx_ELx_Perm11_MASK                            GENMASK(47, 44)
#define PIRx_ELx_Perm11_SHIFT                           44
#define PIRx_ELx_Perm11_WIDTH                           4

#define PIRx_ELx_Perm10                                 GENMASK(43, 40)
#define PIRx_ELx_Perm10_MASK                            GENMASK(43, 40)
#define PIRx_ELx_Perm10_SHIFT                           40
#define PIRx_ELx_Perm10_WIDTH                           4

#define PIRx_ELx_Perm9                                  GENMASK(39, 36)
#define PIRx_ELx_Perm9_MASK                             GENMASK(39, 36)
#define PIRx_ELx_Perm9_SHIFT                            36
#define PIRx_ELx_Perm9_WIDTH                            4

#define PIRx_ELx_Perm8                                  GENMASK(35, 32)
#define PIRx_ELx_Perm8_MASK                             GENMASK(35, 32)
#define PIRx_ELx_Perm8_SHIFT                            32
#define PIRx_ELx_Perm8_WIDTH                            4

#define PIRx_ELx_Perm7                                  GENMASK(31, 28)
#define PIRx_ELx_Perm7_MASK                             GENMASK(31, 28)
#define PIRx_ELx_Perm7_SHIFT                            28
#define PIRx_ELx_Perm7_WIDTH                            4

#define PIRx_ELx_Perm6                                  GENMASK(27, 24)
#define PIRx_ELx_Perm6_MASK                             GENMASK(27, 24)
#define PIRx_ELx_Perm6_SHIFT                            24
#define PIRx_ELx_Perm6_WIDTH                            4

#define PIRx_ELx_Perm5                                  GENMASK(23, 20)
#define PIRx_ELx_Perm5_MASK                             GENMASK(23, 20)
#define PIRx_ELx_Perm5_SHIFT                            20
#define PIRx_ELx_Perm5_WIDTH                            4

#define PIRx_ELx_Perm4                                  GENMASK(19, 16)
#define PIRx_ELx_Perm4_MASK                             GENMASK(19, 16)
#define PIRx_ELx_Perm4_SHIFT                            16
#define PIRx_ELx_Perm4_WIDTH                            4

#define PIRx_ELx_Perm3                                  GENMASK(15, 12)
#define PIRx_ELx_Perm3_MASK                             GENMASK(15, 12)
#define PIRx_ELx_Perm3_SHIFT                            12
#define PIRx_ELx_Perm3_WIDTH                            4

#define PIRx_ELx_Perm2                                  GENMASK(11, 8)
#define PIRx_ELx_Perm2_MASK                             GENMASK(11, 8)
#define PIRx_ELx_Perm2_SHIFT                            8
#define PIRx_ELx_Perm2_WIDTH                            4

#define PIRx_ELx_Perm1                                  GENMASK(7, 4)
#define PIRx_ELx_Perm1_MASK                             GENMASK(7, 4)
#define PIRx_ELx_Perm1_SHIFT                            4
#define PIRx_ELx_Perm1_WIDTH                            4

#define PIRx_ELx_Perm0                                  GENMASK(3, 0)
#define PIRx_ELx_Perm0_MASK                             GENMASK(3, 0)
#define PIRx_ELx_Perm0_SHIFT                            0
#define PIRx_ELx_Perm0_WIDTH                            4

#define PIRx_ELx_RES0                                   (UL(0))
#define PIRx_ELx_RES1                                   (UL(0))
#define PIRx_ELx_UNKN                                   (UL(0))

#define REG_PIRE0_EL1                                   S3_0_C10_C2_2
#define SYS_PIRE0_EL1                                   sys_reg(3, 0, 10, 2, 2)
#define SYS_PIRE0_EL1_Op0                               3
#define SYS_PIRE0_EL1_Op1                               0
#define SYS_PIRE0_EL1_CRn                               10
#define SYS_PIRE0_EL1_CRm                               2
#define SYS_PIRE0_EL1_Op2                               2

/* For PIRE0_EL1 fields see PIRx_ELx */

#define REG_PIRE0_EL12                                  S3_5_C10_C2_2
#define SYS_PIRE0_EL12                                  sys_reg(3, 5, 10, 2, 2)
#define SYS_PIRE0_EL12_Op0                              3
#define SYS_PIRE0_EL12_Op1                              5
#define SYS_PIRE0_EL12_CRn                              10
#define SYS_PIRE0_EL12_CRm                              2
#define SYS_PIRE0_EL12_Op2                              2

/* For PIRE0_EL12 fields see PIRx_ELx */

#define REG_PIR_EL1                                     S3_0_C10_C2_3
#define SYS_PIR_EL1                                     sys_reg(3, 0, 10, 2, 3)
#define SYS_PIR_EL1_Op0                                 3
#define SYS_PIR_EL1_Op1                                 0
#define SYS_PIR_EL1_CRn                                 10
#define SYS_PIR_EL1_CRm                                 2
#define SYS_PIR_EL1_Op2                                 3

/* For PIR_EL1 fields see PIRx_ELx */

#define REG_PIR_EL12                                    S3_5_C10_C2_3
#define SYS_PIR_EL12                                    sys_reg(3, 5, 10, 2, 3)
#define SYS_PIR_EL12_Op0                                3
#define SYS_PIR_EL12_Op1                                5
#define SYS_PIR_EL12_CRn                                10
#define SYS_PIR_EL12_CRm                                2
#define SYS_PIR_EL12_Op2                                3

/* For PIR_EL12 fields see PIRx_ELx */

#define REG_PIR_EL2                                     S3_4_C10_C2_3
#define SYS_PIR_EL2                                     sys_reg(3, 4, 10, 2, 3)
#define SYS_PIR_EL2_Op0                                 3
#define SYS_PIR_EL2_Op1                                 4
#define SYS_PIR_EL2_CRn                                 10
#define SYS_PIR_EL2_CRm                                 2
#define SYS_PIR_EL2_Op2                                 3

/* For PIR_EL2 fields see PIRx_ELx */

#define REG_LORSA_EL1                                   S3_0_C10_C4_0
#define SYS_LORSA_EL1                                   sys_reg(3, 0, 10, 4, 0)
#define SYS_LORSA_EL1_Op0                               3
#define SYS_LORSA_EL1_Op1                               0
#define SYS_LORSA_EL1_CRn                               10
#define SYS_LORSA_EL1_CRm                               4
#define SYS_LORSA_EL1_Op2                               0

#define LORSA_EL1_SA                                    GENMASK(51, 16)
#define LORSA_EL1_SA_MASK                               GENMASK(51, 16)
#define LORSA_EL1_SA_SHIFT                              16
#define LORSA_EL1_SA_WIDTH                              36

#define LORSA_EL1_Valid                                 GENMASK(0, 0)
#define LORSA_EL1_Valid_MASK                            GENMASK(0, 0)
#define LORSA_EL1_Valid_SHIFT                           0
#define LORSA_EL1_Valid_WIDTH                           1

#define LORSA_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 52) | GENMASK_ULL(15, 1))
#define LORSA_EL1_RES1                                  (UL(0))
#define LORSA_EL1_UNKN                                  (UL(0))

#define REG_LOREA_EL1                                   S3_0_C10_C4_1
#define SYS_LOREA_EL1                                   sys_reg(3, 0, 10, 4, 1)
#define SYS_LOREA_EL1_Op0                               3
#define SYS_LOREA_EL1_Op1                               0
#define SYS_LOREA_EL1_CRn                               10
#define SYS_LOREA_EL1_CRm                               4
#define SYS_LOREA_EL1_Op2                               1

#define LOREA_EL1_EA_51_48                              GENMASK(51, 48)
#define LOREA_EL1_EA_51_48_MASK                         GENMASK(51, 48)
#define LOREA_EL1_EA_51_48_SHIFT                        48
#define LOREA_EL1_EA_51_48_WIDTH                        4

#define LOREA_EL1_EA_47_16                              GENMASK(47, 16)
#define LOREA_EL1_EA_47_16_MASK                         GENMASK(47, 16)
#define LOREA_EL1_EA_47_16_SHIFT                        16
#define LOREA_EL1_EA_47_16_WIDTH                        32

#define LOREA_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 52) | GENMASK_ULL(15, 0))
#define LOREA_EL1_RES1                                  (UL(0))
#define LOREA_EL1_UNKN                                  (UL(0))

#define REG_LORN_EL1                                    S3_0_C10_C4_2
#define SYS_LORN_EL1                                    sys_reg(3, 0, 10, 4, 2)
#define SYS_LORN_EL1_Op0                                3
#define SYS_LORN_EL1_Op1                                0
#define SYS_LORN_EL1_CRn                                10
#define SYS_LORN_EL1_CRm                                4
#define SYS_LORN_EL1_Op2                                2

#define LORN_EL1_Num                                    GENMASK(7, 0)
#define LORN_EL1_Num_MASK                               GENMASK(7, 0)
#define LORN_EL1_Num_SHIFT                              0
#define LORN_EL1_Num_WIDTH                              8

#define LORN_EL1_RES0                                   (UL(0) | GENMASK_ULL(63, 8))
#define LORN_EL1_RES1                                   (UL(0))
#define LORN_EL1_UNKN                                   (UL(0))

#define REG_LORC_EL1                                    S3_0_C10_C4_3
#define SYS_LORC_EL1                                    sys_reg(3, 0, 10, 4, 3)
#define SYS_LORC_EL1_Op0                                3
#define SYS_LORC_EL1_Op1                                0
#define SYS_LORC_EL1_CRn                                10
#define SYS_LORC_EL1_CRm                                4
#define SYS_LORC_EL1_Op2                                3

#define LORC_EL1_DS                                     GENMASK(9, 2)
#define LORC_EL1_DS_MASK                                GENMASK(9, 2)
#define LORC_EL1_DS_SHIFT                               2
#define LORC_EL1_DS_WIDTH                               8

#define LORC_EL1_EN                                     GENMASK(0, 0)
#define LORC_EL1_EN_MASK                                GENMASK(0, 0)
#define LORC_EL1_EN_SHIFT                               0
#define LORC_EL1_EN_WIDTH                               1

#define LORC_EL1_RES0                                   (UL(0) | GENMASK_ULL(63, 10) | GENMASK_ULL(1, 1))
#define LORC_EL1_RES1                                   (UL(0))
#define LORC_EL1_UNKN                                   (UL(0))

#define REG_LORID_EL1                                   S3_0_C10_C4_7
#define SYS_LORID_EL1                                   sys_reg(3, 0, 10, 4, 7)
#define SYS_LORID_EL1_Op0                               3
#define SYS_LORID_EL1_Op1                               0
#define SYS_LORID_EL1_CRn                               10
#define SYS_LORID_EL1_CRm                               4
#define SYS_LORID_EL1_Op2                               7

#define LORID_EL1_LD                                    GENMASK(23, 16)
#define LORID_EL1_LD_MASK                               GENMASK(23, 16)
#define LORID_EL1_LD_SHIFT                              16
#define LORID_EL1_LD_WIDTH                              8

#define LORID_EL1_LR                                    GENMASK(7, 0)
#define LORID_EL1_LR_MASK                               GENMASK(7, 0)
#define LORID_EL1_LR_SHIFT                              0
#define LORID_EL1_LR_WIDTH                              8

#define LORID_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 24) | GENMASK_ULL(15, 8))
#define LORID_EL1_RES1                                  (UL(0))
#define LORID_EL1_UNKN                                  (UL(0))

#define REG_ISR_EL1                                     S3_0_C12_C1_0
#define SYS_ISR_EL1                                     sys_reg(3, 0, 12, 1, 0)
#define SYS_ISR_EL1_Op0                                 3
#define SYS_ISR_EL1_Op1                                 0
#define SYS_ISR_EL1_CRn                                 12
#define SYS_ISR_EL1_CRm                                 1
#define SYS_ISR_EL1_Op2                                 0

#define ISR_EL1_IS                                      GENMASK(10, 10)
#define ISR_EL1_IS_MASK                                 GENMASK(10, 10)
#define ISR_EL1_IS_SHIFT                                10
#define ISR_EL1_IS_WIDTH                                1

#define ISR_EL1_FS                                      GENMASK(9, 9)
#define ISR_EL1_FS_MASK                                 GENMASK(9, 9)
#define ISR_EL1_FS_SHIFT                                9
#define ISR_EL1_FS_WIDTH                                1

#define ISR_EL1_A                                       GENMASK(8, 8)
#define ISR_EL1_A_MASK                                  GENMASK(8, 8)
#define ISR_EL1_A_SHIFT                                 8
#define ISR_EL1_A_WIDTH                                 1

#define ISR_EL1_I                                       GENMASK(7, 7)
#define ISR_EL1_I_MASK                                  GENMASK(7, 7)
#define ISR_EL1_I_SHIFT                                 7
#define ISR_EL1_I_WIDTH                                 1

#define ISR_EL1_F                                       GENMASK(6, 6)
#define ISR_EL1_F_MASK                                  GENMASK(6, 6)
#define ISR_EL1_F_SHIFT                                 6
#define ISR_EL1_F_WIDTH                                 1

#define ISR_EL1_RES0                                    (UL(0) | GENMASK_ULL(63, 11) | GENMASK_ULL(5, 0))
#define ISR_EL1_RES1                                    (UL(0))
#define ISR_EL1_UNKN                                    (UL(0))

#define REG_ICC_NMIAR1_EL1                              S3_0_C12_C9_5
#define SYS_ICC_NMIAR1_EL1                              sys_reg(3, 0, 12, 9, 5)
#define SYS_ICC_NMIAR1_EL1_Op0                          3
#define SYS_ICC_NMIAR1_EL1_Op1                          0
#define SYS_ICC_NMIAR1_EL1_CRn                          12
#define SYS_ICC_NMIAR1_EL1_CRm                          9
#define SYS_ICC_NMIAR1_EL1_Op2                          5

#define ICC_NMIAR1_EL1_INTID                            GENMASK(23, 0)
#define ICC_NMIAR1_EL1_INTID_MASK                       GENMASK(23, 0)
#define ICC_NMIAR1_EL1_INTID_SHIFT                      0
#define ICC_NMIAR1_EL1_INTID_WIDTH                      24

#define ICC_NMIAR1_EL1_RES0                             (UL(0) | GENMASK_ULL(63, 24))
#define ICC_NMIAR1_EL1_RES1                             (UL(0))
#define ICC_NMIAR1_EL1_UNKN                             (UL(0))

#define REG_TRBLIMITR_EL1                               S3_0_C9_C11_0
#define SYS_TRBLIMITR_EL1                               sys_reg(3, 0, 9, 11, 0)
#define SYS_TRBLIMITR_EL1_Op0                           3
#define SYS_TRBLIMITR_EL1_Op1                           0
#define SYS_TRBLIMITR_EL1_CRn                           9
#define SYS_TRBLIMITR_EL1_CRm                           11
#define SYS_TRBLIMITR_EL1_Op2                           0

#define TRBLIMITR_EL1_LIMIT                             GENMASK(63, 12)
#define TRBLIMITR_EL1_LIMIT_MASK                        GENMASK(63, 12)
#define TRBLIMITR_EL1_LIMIT_SHIFT                       12
#define TRBLIMITR_EL1_LIMIT_WIDTH                       52

#define TRBLIMITR_EL1_XE                                GENMASK(6, 6)
#define TRBLIMITR_EL1_XE_MASK                           GENMASK(6, 6)
#define TRBLIMITR_EL1_XE_SHIFT                          6
#define TRBLIMITR_EL1_XE_WIDTH                          1

#define TRBLIMITR_EL1_nVM                               GENMASK(5, 5)
#define TRBLIMITR_EL1_nVM_MASK                          GENMASK(5, 5)
#define TRBLIMITR_EL1_nVM_SHIFT                         5
#define TRBLIMITR_EL1_nVM_WIDTH                         1

#define TRBLIMITR_EL1_TM                                GENMASK(4, 3)
#define TRBLIMITR_EL1_TM_MASK                           GENMASK(4, 3)
#define TRBLIMITR_EL1_TM_SHIFT                          3
#define TRBLIMITR_EL1_TM_WIDTH                          2
#define TRBLIMITR_EL1_TM_STOP                           UL(0b00)
#define TRBLIMITR_EL1_TM_IRQ                            UL(0b01)
#define TRBLIMITR_EL1_TM_IGNR                           UL(0b11)

#define TRBLIMITR_EL1_FM                                GENMASK(2, 1)
#define TRBLIMITR_EL1_FM_MASK                           GENMASK(2, 1)
#define TRBLIMITR_EL1_FM_SHIFT                          1
#define TRBLIMITR_EL1_FM_WIDTH                          2
#define TRBLIMITR_EL1_FM_FILL                           UL(0b00)
#define TRBLIMITR_EL1_FM_WRAP                           UL(0b01)
#define TRBLIMITR_EL1_FM_CBUF                           UL(0b11)

#define TRBLIMITR_EL1_E                                 GENMASK(0, 0)
#define TRBLIMITR_EL1_E_MASK                            GENMASK(0, 0)
#define TRBLIMITR_EL1_E_SHIFT                           0
#define TRBLIMITR_EL1_E_WIDTH                           1

#define TRBLIMITR_EL1_RES0                              (UL(0) | GENMASK_ULL(11, 7))
#define TRBLIMITR_EL1_RES1                              (UL(0))
#define TRBLIMITR_EL1_UNKN                              (UL(0))

#define REG_TRBPTR_EL1                                  S3_0_C9_C11_1
#define SYS_TRBPTR_EL1                                  sys_reg(3, 0, 9, 11, 1)
#define SYS_TRBPTR_EL1_Op0                              3
#define SYS_TRBPTR_EL1_Op1                              0
#define SYS_TRBPTR_EL1_CRn                              9
#define SYS_TRBPTR_EL1_CRm                              11
#define SYS_TRBPTR_EL1_Op2                              1

#define TRBPTR_EL1_PTR                                  GENMASK(63, 0)
#define TRBPTR_EL1_PTR_MASK                             GENMASK(63, 0)
#define TRBPTR_EL1_PTR_SHIFT                            0
#define TRBPTR_EL1_PTR_WIDTH                            64

#define TRBPTR_EL1_RES0                                 (UL(0))
#define TRBPTR_EL1_RES1                                 (UL(0))
#define TRBPTR_EL1_UNKN                                 (UL(0))

#define REG_TRBBASER_EL1                                S3_0_C9_C11_2
#define SYS_TRBBASER_EL1                                sys_reg(3, 0, 9, 11, 2)
#define SYS_TRBBASER_EL1_Op0                            3
#define SYS_TRBBASER_EL1_Op1                            0
#define SYS_TRBBASER_EL1_CRn                            9
#define SYS_TRBBASER_EL1_CRm                            11
#define SYS_TRBBASER_EL1_Op2                            2

#define TRBBASER_EL1_BASE                               GENMASK(63, 12)
#define TRBBASER_EL1_BASE_MASK                          GENMASK(63, 12)
#define TRBBASER_EL1_BASE_SHIFT                         12
#define TRBBASER_EL1_BASE_WIDTH                         52

#define TRBBASER_EL1_RES0                               (UL(0) | GENMASK_ULL(11, 0))
#define TRBBASER_EL1_RES1                               (UL(0))
#define TRBBASER_EL1_UNKN                               (UL(0))

#define REG_TRBSR_EL1                                   S3_0_C9_C11_3
#define SYS_TRBSR_EL1                                   sys_reg(3, 0, 9, 11, 3)
#define SYS_TRBSR_EL1_Op0                               3
#define SYS_TRBSR_EL1_Op1                               0
#define SYS_TRBSR_EL1_CRn                               9
#define SYS_TRBSR_EL1_CRm                               11
#define SYS_TRBSR_EL1_Op2                               3

#define TRBSR_EL1_MSS2                                  GENMASK(55, 32)
#define TRBSR_EL1_MSS2_MASK                             GENMASK(55, 32)
#define TRBSR_EL1_MSS2_SHIFT                            32
#define TRBSR_EL1_MSS2_WIDTH                            24

#define TRBSR_EL1_EC                                    GENMASK(31, 26)
#define TRBSR_EL1_EC_MASK                               GENMASK(31, 26)
#define TRBSR_EL1_EC_SHIFT                              26
#define TRBSR_EL1_EC_WIDTH                              6

#define TRBSR_EL1_DAT                                   GENMASK(23, 23)
#define TRBSR_EL1_DAT_MASK                              GENMASK(23, 23)
#define TRBSR_EL1_DAT_SHIFT                             23
#define TRBSR_EL1_DAT_WIDTH                             1

#define TRBSR_EL1_IRQ                                   GENMASK(22, 22)
#define TRBSR_EL1_IRQ_MASK                              GENMASK(22, 22)
#define TRBSR_EL1_IRQ_SHIFT                             22
#define TRBSR_EL1_IRQ_WIDTH                             1

#define TRBSR_EL1_TRG                                   GENMASK(21, 21)
#define TRBSR_EL1_TRG_MASK                              GENMASK(21, 21)
#define TRBSR_EL1_TRG_SHIFT                             21
#define TRBSR_EL1_TRG_WIDTH                             1

#define TRBSR_EL1_WRAP                                  GENMASK(20, 20)
#define TRBSR_EL1_WRAP_MASK                             GENMASK(20, 20)
#define TRBSR_EL1_WRAP_SHIFT                            20
#define TRBSR_EL1_WRAP_WIDTH                            1

#define TRBSR_EL1_EA                                    GENMASK(18, 18)
#define TRBSR_EL1_EA_MASK                               GENMASK(18, 18)
#define TRBSR_EL1_EA_SHIFT                              18
#define TRBSR_EL1_EA_WIDTH                              1

#define TRBSR_EL1_S                                     GENMASK(17, 17)
#define TRBSR_EL1_S_MASK                                GENMASK(17, 17)
#define TRBSR_EL1_S_SHIFT                               17
#define TRBSR_EL1_S_WIDTH                               1

#define TRBSR_EL1_MSS                                   GENMASK(15, 0)
#define TRBSR_EL1_MSS_MASK                              GENMASK(15, 0)
#define TRBSR_EL1_MSS_SHIFT                             0
#define TRBSR_EL1_MSS_WIDTH                             16

#define TRBSR_EL1_RES0                                  (UL(0) | GENMASK_ULL(63, 56) | GENMASK_ULL(25, 24) | GENMASK_ULL(19, 19) | GENMASK_ULL(16, 16))
#define TRBSR_EL1_RES1                                  (UL(0))
#define TRBSR_EL1_UNKN                                  (UL(0))

#define REG_TRBMAR_EL1                                  S3_0_C9_C11_4
#define SYS_TRBMAR_EL1                                  sys_reg(3, 0, 9, 11, 4)
#define SYS_TRBMAR_EL1_Op0                              3
#define SYS_TRBMAR_EL1_Op1                              0
#define SYS_TRBMAR_EL1_CRn                              9
#define SYS_TRBMAR_EL1_CRm                              11
#define SYS_TRBMAR_EL1_Op2                              4

#define TRBMAR_EL1_PAS                                  GENMASK(11, 10)
#define TRBMAR_EL1_PAS_MASK                             GENMASK(11, 10)
#define TRBMAR_EL1_PAS_SHIFT                            10
#define TRBMAR_EL1_PAS_WIDTH                            2
#define TRBMAR_EL1_PAS_SECURE                           UL(0b00)
#define TRBMAR_EL1_PAS_NON_SECURE                       UL(0b01)
#define TRBMAR_EL1_PAS_ROOT                             UL(0b10)
#define TRBMAR_EL1_PAS_REALM                            UL(0b11)

#define TRBMAR_EL1_SH                                   GENMASK(9, 8)
#define TRBMAR_EL1_SH_MASK                              GENMASK(9, 8)
#define TRBMAR_EL1_SH_SHIFT                             8
#define TRBMAR_EL1_SH_WIDTH                             2
#define TRBMAR_EL1_SH_NON_SHAREABLE                     UL(0b00)
#define TRBMAR_EL1_SH_OUTER_SHAREABLE                   UL(0b10)
#define TRBMAR_EL1_SH_INNER_SHAREABLE                   UL(0b11)

#define TRBMAR_EL1_Attr                                 GENMASK(7, 0)
#define TRBMAR_EL1_Attr_MASK                            GENMASK(7, 0)
#define TRBMAR_EL1_Attr_SHIFT                           0
#define TRBMAR_EL1_Attr_WIDTH                           8

#define TRBMAR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 12))
#define TRBMAR_EL1_RES1                                 (UL(0))
#define TRBMAR_EL1_UNKN                                 (UL(0))

#define REG_TRBTRG_EL1                                  S3_0_C9_C11_6
#define SYS_TRBTRG_EL1                                  sys_reg(3, 0, 9, 11, 6)
#define SYS_TRBTRG_EL1_Op0                              3
#define SYS_TRBTRG_EL1_Op1                              0
#define SYS_TRBTRG_EL1_CRn                              9
#define SYS_TRBTRG_EL1_CRm                              11
#define SYS_TRBTRG_EL1_Op2                              6

#define TRBTRG_EL1_TRG                                  GENMASK(31, 0)
#define TRBTRG_EL1_TRG_MASK                             GENMASK(31, 0)
#define TRBTRG_EL1_TRG_SHIFT                            0
#define TRBTRG_EL1_TRG_WIDTH                            32

#define TRBTRG_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 32))
#define TRBTRG_EL1_RES1                                 (UL(0))
#define TRBTRG_EL1_UNKN                                 (UL(0))

#define REG_TRBIDR_EL1                                  S3_0_C9_C11_7
#define SYS_TRBIDR_EL1                                  sys_reg(3, 0, 9, 11, 7)
#define SYS_TRBIDR_EL1_Op0                              3
#define SYS_TRBIDR_EL1_Op1                              0
#define SYS_TRBIDR_EL1_CRn                              9
#define SYS_TRBIDR_EL1_CRm                              11
#define SYS_TRBIDR_EL1_Op2                              7

#define TRBIDR_EL1_EA                                   GENMASK(11, 8)
#define TRBIDR_EL1_EA_MASK                              GENMASK(11, 8)
#define TRBIDR_EL1_EA_SHIFT                             8
#define TRBIDR_EL1_EA_WIDTH                             4
#define TRBIDR_EL1_EA_NON_DESC                          UL(0b0000)
#define TRBIDR_EL1_EA_IGNORE                            UL(0b0001)
#define TRBIDR_EL1_EA_SERROR                            UL(0b0010)

#define TRBIDR_EL1_F                                    GENMASK(5, 5)
#define TRBIDR_EL1_F_MASK                               GENMASK(5, 5)
#define TRBIDR_EL1_F_SHIFT                              5
#define TRBIDR_EL1_F_WIDTH                              1

#define TRBIDR_EL1_P                                    GENMASK(4, 4)
#define TRBIDR_EL1_P_MASK                               GENMASK(4, 4)
#define TRBIDR_EL1_P_SHIFT                              4
#define TRBIDR_EL1_P_WIDTH                              1

#define TRBIDR_EL1_Align                                GENMASK(3, 0)
#define TRBIDR_EL1_Align_MASK                           GENMASK(3, 0)
#define TRBIDR_EL1_Align_SHIFT                          0
#define TRBIDR_EL1_Align_WIDTH                          4

#define TRBIDR_EL1_RES0                                 (UL(0) | GENMASK_ULL(63, 12) | GENMASK_ULL(7, 6))
#define TRBIDR_EL1_RES1                                 (UL(0))
#define TRBIDR_EL1_UNKN                                 (UL(0))

#endif /* __ASM_SYSREG_DEFS_H */
