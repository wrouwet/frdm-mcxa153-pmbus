/*
 * PMBus command codes and status bits, from PMBus Power System Management Protocol
 * Specification Part II, revision 1.3.1. Only the commands this project implements or
 * refers to are listed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PMBUS_PMBUS_H_
#define PMBUS_PMBUS_H_

#define PMBUS_PAGE                0x00
#define PMBUS_OPERATION           0x01
#define PMBUS_ON_OFF_CONFIG       0x02
#define PMBUS_CLEAR_FAULTS        0x03
#define PMBUS_WRITE_PROTECT       0x10
#define PMBUS_STORE_DEFAULT_ALL   0x11
#define PMBUS_RESTORE_DEFAULT_ALL 0x12
#define PMBUS_CAPABILITY          0x19
#define PMBUS_QUERY               0x1A
#define PMBUS_VOUT_MODE           0x20
#define PMBUS_VOUT_COMMAND        0x21
#define PMBUS_VOUT_MARGIN_HIGH    0x25
#define PMBUS_VOUT_MARGIN_LOW     0x26
#define PMBUS_VOUT_OV_FAULT_LIMIT 0x40
#define PMBUS_VOUT_OV_WARN_LIMIT  0x42
#define PMBUS_VOUT_UV_WARN_LIMIT  0x43
#define PMBUS_VOUT_UV_FAULT_LIMIT 0x44
#define PMBUS_IOUT_OC_FAULT_LIMIT 0x46
#define PMBUS_IOUT_OC_WARN_LIMIT  0x4A
#define PMBUS_OT_FAULT_LIMIT      0x4F
#define PMBUS_OT_WARN_LIMIT       0x51
#define PMBUS_UT_WARN_LIMIT       0x52
#define PMBUS_UT_FAULT_LIMIT      0x53
#define PMBUS_VIN_OV_FAULT_LIMIT  0x55
#define PMBUS_VIN_OV_WARN_LIMIT   0x57
#define PMBUS_VIN_UV_WARN_LIMIT   0x58
#define PMBUS_VIN_UV_FAULT_LIMIT  0x59
#define PMBUS_STATUS_BYTE         0x78
#define PMBUS_STATUS_WORD         0x79
#define PMBUS_STATUS_VOUT         0x7A
#define PMBUS_STATUS_IOUT         0x7B
#define PMBUS_STATUS_INPUT        0x7C
#define PMBUS_STATUS_TEMPERATURE  0x7D
#define PMBUS_STATUS_CML          0x7E
#define PMBUS_STATUS_MFR_SPECIFIC 0x80
#define PMBUS_READ_VIN            0x88
#define PMBUS_READ_IIN            0x89
#define PMBUS_READ_VOUT           0x8B
#define PMBUS_READ_IOUT           0x8C
#define PMBUS_READ_TEMPERATURE_1  0x8D
#define PMBUS_READ_TEMPERATURE_2  0x8E
#define PMBUS_READ_POUT           0x96
#define PMBUS_READ_PIN            0x97
#define PMBUS_PMBUS_REVISION      0x98
#define PMBUS_MFR_ID              0x99
#define PMBUS_MFR_MODEL           0x9A
#define PMBUS_MFR_REVISION        0x9B
#define PMBUS_MFR_LOCATION        0x9C
#define PMBUS_MFR_DATE            0x9D
#define PMBUS_MFR_SERIAL          0x9E
#define PMBUS_MFR_SPECIFIC_00     0xD0
#define PMBUS_MFR_SPECIFIC_01     0xD1

/* OPERATION */
#define PMBUS_OPERATION_ON          0x80
#define PMBUS_OPERATION_MARGIN_MASK 0x30
#define PMBUS_OPERATION_MARGIN_LOW  0x10
#define PMBUS_OPERATION_MARGIN_HIGH 0x20

/* ON_OFF_CONFIG */
#define PMBUS_ON_OFF_PU  0x10 /* unit needs a command or the CONTROL pin to turn on */
#define PMBUS_ON_OFF_CMD 0x08 /* unit obeys the OPERATION on bit */
#define PMBUS_ON_OFF_CPR 0x04 /* unit needs the CONTROL pin */

/* WRITE_PROTECT */
#define PMBUS_WP_ALL      0x80 /* everything except WRITE_PROTECT */
#define PMBUS_WP_OP       0x40 /* everything except WRITE_PROTECT, OPERATION, PAGE */
#define PMBUS_WP_VOUT_CMD 0x20 /* ...also except ON_OFF_CONFIG and VOUT_COMMAND */

/* CAPABILITY */
#define PMBUS_CAP_PEC      0x80
#define PMBUS_CAP_400KHZ   0x20
#define PMBUS_CAP_SMBALERT 0x10

/* QUERY response */
#define PMBUS_QUERY_SUPPORTED   0x80
#define PMBUS_QUERY_WRITE       0x40
#define PMBUS_QUERY_READ        0x20
#define PMBUS_QUERY_FMT_LINEAR  (0 << 2)
#define PMBUS_QUERY_FMT_U8      (4 << 2)
#define PMBUS_QUERY_FMT_NONNUM  (7 << 2)

/* STATUS_BYTE, the low byte of STATUS_WORD */
#define PMBUS_SB_BUSY           0x80
#define PMBUS_SB_OFF            0x40
#define PMBUS_SB_VOUT_OV_FAULT  0x20
#define PMBUS_SB_IOUT_OC_FAULT  0x10
#define PMBUS_SB_VIN_UV_FAULT   0x08
#define PMBUS_SB_TEMPERATURE    0x04
#define PMBUS_SB_CML            0x02
#define PMBUS_SB_NONE_OF_ABOVE  0x01

/* The high byte of STATUS_WORD */
#define PMBUS_SW_VOUT           0x8000
#define PMBUS_SW_IOUT_POUT      0x4000
#define PMBUS_SW_INPUT          0x2000
#define PMBUS_SW_MFR            0x1000
#define PMBUS_SW_POWER_GOOD_N   0x0800

/* STATUS_VOUT */
#define PMBUS_SV_OV_FAULT 0x80
#define PMBUS_SV_OV_WARN  0x40
#define PMBUS_SV_UV_WARN  0x20
#define PMBUS_SV_UV_FAULT 0x10

/* STATUS_IOUT */
#define PMBUS_SI_OC_FAULT 0x80
#define PMBUS_SI_OC_WARN  0x20

/* STATUS_INPUT */
#define PMBUS_SIN_OV_FAULT   0x80
#define PMBUS_SIN_OV_WARN    0x40
#define PMBUS_SIN_UV_WARN    0x20
#define PMBUS_SIN_UV_FAULT   0x10
#define PMBUS_SIN_OFF_LOW_IN 0x08

/* STATUS_TEMPERATURE */
#define PMBUS_ST_OT_FAULT 0x80
#define PMBUS_ST_OT_WARN  0x40
#define PMBUS_ST_UT_WARN  0x20
#define PMBUS_ST_UT_FAULT 0x10

/* STATUS_CML */
#define PMBUS_CML_INVALID_CMD  0x80
#define PMBUS_CML_INVALID_DATA 0x40
#define PMBUS_CML_PEC_FAILED   0x20
#define PMBUS_CML_MEMORY_FAULT 0x10
#define PMBUS_CML_OTHER_COMM   0x02

/* PMBUS_REVISION: Part I and Part II revision 1.3 */
#define PMBUS_REVISION_1_3 0x33

/* The SMBus Alert Response Address. */
#define SMBUS_ALERT_RESPONSE_ADDR 0x0C

#endif /* PMBUS_PMBUS_H_ */
