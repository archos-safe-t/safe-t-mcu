/*
 * Interface to cryptomem
 *
 * High level functions
 *
 */
#if CRYPTOMEM
#include <stdint.h>
#include <string.h>
#include "at88sc0104.h"
#include "rng.h"

#include <libopencm3/stm32/flash.h>

#ifndef FALSE
#define FALSE       (0)
#define TRUE        (!FALSE)
#endif


#define OTP_START_ADDR		(0x1FFF7800U)
#define OTP_LOCK_ADDR		(0x1FFF7A00U)

/*!
 *
 * \brief 	Store the seed for the cryptomem in OTP part of the CPU
 *
 * \note 	CPU specific. Can be called only once (because it locks the OTP)
 * 			OTP Block 1 is used (out of 16 possible)
 *
 * \param	seed: 4 sets of crypto seeds
 *
 * returns CM_SUCCESS
 */
uint8_t cm_store_seed_in_OTP(uint8_t seed[4][8])
{
	/* this code needs write access to 0x1FFF78xx - so use with MPU disabled
	 * or before calling config_mpu() ! */

	/* store seed in OTP Block 1 */
#ifdef STM32F2
	/* code is processor specific! */
	flash_unlock();
	for (int i = 0; i < 4; i++) {
		for (int l=0; l<8; l++)
			flash_program_byte(OTP_START_ADDR + 0x20 + l + i*8, seed[i][l]);
	}

	/* set LOCK byte for OTP Block 1 */
	flash_program_byte(OTP_START_ADDR + 0x200 + 0x01, 0x00);

	flash_lock();
	return CM_SUCCESS;
#else
	return CM_FAIL;
#endif
}

/*!
 *
 * \brief 	Read the seed for the cryptomem authentication
 *
 * \param	seed: will contain a copy of the seed
 * \param	index: index of the seed (0..3)
 *
 * returns CM_SUCCESS
 */
uint8_t cm_get_seed_in_OTP(uint8_t *seed, uint8_t index )
{
	memcpy( seed, (const uint8_t *)OTP_START_ADDR + 0x20 + index*8, 8);
	return CM_SUCCESS;
}

/*!
 *
 * \brief 	Check if the initial programming of the cryptomem is ok
 *
 * \note 	Can be called during production to verify the initialization is ok
 *
 * \param	seed: 4 sets of crypto seeds
 *
 * returns CM_SUCCESS if cryptomem is correctly inialized
 */
uint8_t cm_check_programming(uint8_t seed[4][8])
{
	cm_PowerOn();

	uint8_t ret;

	/* disable authentication - just in case... */
	if ((ret = cm_DeactivateSecurity()) != CM_SUCCESS) {
		return ret;
	}

	uint8_t attempts;

	/* check PW7 locked */
	if ((ret = cm_CheckPAC(7, CM_PWWRITE, &attempts)) != CM_SUCCESS || attempts != 0) {
		return CM_FAILED;
	}

	/* check PW0..3 should have 4 attempts still */
	for (int i=0; i<4; i++) {
		if ((ret = cm_CheckPAC(i, CM_PWWRITE, &attempts)) != CM_SUCCESS || attempts != 4) {
			return CM_FAILED;
		}
	}

	/* check the 4 seeds and passwords */
	uint8_t fuse = 0xFF;

	for (int i=0; i<4; i++) {
		uint8_t rnd_buffer[16];

		random_buffer(rnd_buffer, 16);

		ret = cm_ActivateSecurity( i, seed[i], rnd_buffer, 1); // encryption
		if (ret != CM_SUCCESS)
			return ret;

		uint8_t default_pw[8] = { 0xFF, 0xFF, 0xFF };
		ret = cm_VerifyPassword(default_pw, i, CM_PWWRITE);
		if (ret != CM_SUCCESS)
			return ret;

		if (i == 3) { // on last iteration read the fuse
		    ret = cm_ReadFuse(&fuse);
		    if (ret != CM_SUCCESS)
				return ret;
		}

		if ((ret = cm_DeactivateSecurity()) != CM_SUCCESS) {
			return ret;
		}
	}

	/* check fuse */
	if (fuse != 0)
		return CM_FAILED;

	return ret;
}

/*!
 *
 * \brief 	Do the initial programming of the cryptomem
 *
 * \param	seed: 4 sets of crypto seeds
 *
 * returns CM_SUCCESS if ok
 * returns CM_ALREADY_PGMD if the chip is already programmed
 */
uint8_t cm_init_manufacturing(uint8_t seed[4][8])
{
	// FIXME: this needs to be called from prodtest during manufacturing

	cm_PowerOn();

	uint8_t ret = cm_aCommunicationTest();
	if (ret != CM_SUCCESS) {
		return ret;
	}

	/* disable authentication - just in case... */
	if ((ret = cm_DeactivateSecurity()) != CM_SUCCESS) {
		return ret;
	}

	/* check if already programmed */
	uint8_t read_buffer[4];

	if ((ret = cm_ReadConfigZone(0x0C, read_buffer, 4)) != CM_SUCCESS) {
		return ret;
	}
	if (memcmp(read_buffer, (uint8_t[] ) { 0x40, 0x53, 0x30, 0x00 }, 4) == 0){ // is Mfg Code  "AS0"?
		return CM_ALREADY_PGMD;
	}

	/* Verify security password  - table 6-3 AT88SC0104CA DD 42 97 */
	/* this is PW 7 "secure code" - it allows "access all areas" */
	uint8_t write_buffer[8] = { 0xDD, 0x42, 0x97 };
	if ((ret = cm_VerifySecurePasswd(write_buffer)) != CM_SUCCESS) {
		return ret;
	}

	/* set passwords to all ones */

	memset(write_buffer, 0xFF, 3);

	for (int i = 0; i < 7; i++) {
		// write Pw
		if ((ret = cm_WriteConfigZone(CM_PSW_ADDR + (i << 3) + 1, write_buffer,
				3, TRUE)) != CM_SUCCESS) {
			return ret;
		}
		// read PW
		if ((ret = cm_WriteConfigZone(CM_PSW_ADDR + (i << 3) + 5, write_buffer,
				3, TRUE)) != CM_SUCCESS) {
			return ret;
		}
	}

	for (int i = 0; i < 4; i++) {
		if ((ret = cm_WriteConfigZone(CM_G_ADDR + (i << 3), seed[i], 8, TRUE))
				!= CM_SUCCESS) {
			return ret;
		}

		/* set zone configuration */
		uint8_t AR = 0x57; // R/W PW, R/W auth, encryption

		uint8_t PR = (i << 6) | (i << 4) | i;	  // auth key i, POK key i, pw i

		if ((ret = cm_WriteConfigZone(CM_AR_ADDR + (i << 1), &AR, 1, TRUE))
				!= CM_SUCCESS) {
			return ret;
		}

		if ((ret = cm_WriteConfigZone(CM_PR_ADDR + (i << 1), &PR, 1, TRUE))
				!= CM_SUCCESS) {
			return ret;
		}

		if ((ret = cm_ReadConfigZone(CM_AR_ADDR + (i << 1), &AR, 1))
				!= CM_SUCCESS) {
			return ret;
		}

		if ((ret = cm_ReadConfigZone(CM_PR_ADDR + (i << 1), &PR, 1))
				!= CM_SUCCESS) {
			return ret;
		}

	}

	memcpy(write_buffer, (uint8_t[] ) { 0x40, 0x53, 0x30, 0x00 }, 4); // set Mfg Code to "AS0"
	if ((ret = cm_WriteConfigZone(0x0C, write_buffer, 4, TRUE)) != CM_SUCCESS) {
		return ret;
	}

	/* set Security Fuses */
	ret = cm_BurnFuse(CM_FAB);
	if (ret != CM_SUCCESS)
		return ret;

	ret = cm_BurnFuse(CM_CMA);
	if (ret != CM_SUCCESS)
		return ret;

	ret = cm_BurnFuse(CM_PER);
	if (ret != CM_SUCCESS)
		return ret;

	if ((ret = cm_DeactivateSecurity()) != CM_SUCCESS) {
			while(1);
	}

	/* disable PW7 by abusing it 4 times */

	memset(write_buffer, 0, 8);
	cm_VerifySecurePasswd(write_buffer);
	cm_VerifySecurePasswd(write_buffer);
	cm_VerifySecurePasswd(write_buffer);
	cm_VerifySecurePasswd(write_buffer);

	return ret;
}

uint8_t cm_prodtest_communication_test(void)
{
	cm_PowerOn();

	return cm_aCommunicationTest();
}

uint8_t cm_prodtest_initialization(void)
{
	uint8_t ret;
	uint8_t crypto_seed[4][8];
	uint8_t i = 0, l = 0;

	/* generate random seed using hardware RNG */
	for (i = 0; i < 4; i++) {
		for (l = 0; l < 8; l += 4) {
			uint32_t r = random32();
			memcpy((uint8_t *) &crypto_seed[i] + l, &r, 4);
		}
	}

	ret = cm_init_manufacturing(crypto_seed);
	if (ret == CM_SUCCESS) {
		/* store in OTP, too */
		ret = cm_store_seed_in_OTP(crypto_seed);
		if (ret == CM_FAILED) {
			return ret;
		}
	} else {
		if (ret == CM_ALREADY_PGMD) {
			/* just check */
			uint8_t seed[4][8];
			for (i = 0; i < 4; i++) {
				cm_get_seed_in_OTP(seed[i], i);
			}

			ret = cm_check_programming(seed);
			if (ret == 0) {
				return CM_SUCCESS;
			} else {
				return CM_FAILED;
			}
		}
	}
	return CM_SUCCESS;
}

#endif /* CRYPTMEM */