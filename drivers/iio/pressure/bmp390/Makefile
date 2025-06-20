
#
# Makefile for Bosch BMP390 sensor driver.
#
# Define CHECK flag to enable additional checks in the driver code
ccflags-y += -DCHECK=1
# Conditional block to handle SPI bus support
ifeq ($(CONFIG_BMP3_SUPPORT_SPI_BUS), y)
	ccflags-y += -DBMP3_SUPPORT_SPI_BUS
# Conditional block for dynamic kernel module support
	ifeq ($(CONFIG_BMP3_SUPPORT_DYNAMIC_KO_MODULE), y)
		SRCS   = bmp390_spi.c bmp3.c bmp390_driver.c bmp3_selftest.c bmp390_iio_buffer.c
		OBJS   = $(SRCS:.c=.o)
		obj-m += bmp390_spi_driver.o

		bmp390_spi_driver-objs := bmp390_spi.o bmp3.o bmp390_driver.o bmp3_selftest.o bmp390_iio_buffer.o
	else
		obj-y 	+= bmp390_spi.o bmp3.o bmp390_driver.o bmp3_selftest.o bmp390_iio_buffer.o
	endif
# Conditional block for I2C bus support
else
# Conditional block for dynamic kernel module support
	ifeq ($(CONFIG_BMP3_SUPPORT_DYNAMIC_KO_MODULE), y)
		SRCS   = bmp390_i2c.c bmp3.c bmp390_driver.c bmp3_selftest.c bmp390_iio_buffer.c
		OBJS   = $(SRCS:.c=.o)
		obj-m += bmp390_i2c_driver.o

		bmp390_i2c_driver-objs := bmp390_i2c.o bmp3.o bmp390_driver.o bmp3_selftest.o bmp390_iio_buffer.o
	else
		obj-y 	+= bmp390_i2c.o bmp3.o bmp390_driver.o bmp3_selftest.o bmp390_iio_buffer.o
	endif
endif
