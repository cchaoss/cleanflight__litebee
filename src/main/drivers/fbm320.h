#ifndef _FBM320_H
#define _FBM320_H

#define SDO_Level	0

#if 	SDO_Level == 0
#define SDO_Addr	0x00
#elif	SDO_Level == 1
#define SDO_Addr	0x01
#endif
 
#define FMTISensorAdd_SPI	0x0
#define FMTISensorAdd_I2C	0x6C | SDO_Addr


#define sample_count 21
#define sample_count_max 48
#define pressure_sample_count (sample_count - 1)


struct Sensor
{
	int32_t UP;
	int32_t UT;
	int32_t RP;
	int32_t RT;
	int32_t Reff_P;
	int32_t Altitude;
	uint16_t C0, C1, C2, C3, C6, C8, C9, C10, C11, C12; 
	uint32_t C4, C5, C7;
};
extern struct Sensor FB;

void fbm320_init(void);
void start_temperature(void);
void start_pressure(void);
uint32_t Read_data(void);
void read_offset(void);
void calculate_real_data(int32_t UP, int32_t UT);


float Rel_Altitude(int32_t Press, int32_t Ref_P);
int32_t Abs_Altitude(int32_t Press);

//int32_t FBW320_calculate_Altitude(void);

#endif
