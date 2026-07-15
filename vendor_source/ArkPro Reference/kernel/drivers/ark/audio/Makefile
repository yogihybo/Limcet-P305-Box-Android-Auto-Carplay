# ARK Platform support
snd-soc-ark-pcm-dma-objs 				:= ark_pcm_dma.o
snd-soc-ark-i2s-objs 					:= ark_i2s.o

obj-$(CONFIG_SND_ARK_SOC_DMA) 			+= snd-soc-ark-pcm-dma.o
obj-$(CONFIG_SND_ARK_SOC_I2S) 			+= snd-soc-ark-i2s.o

# ARK codec suuport
snd-soc-sddac-objs						:= ark-sddac-codec.o ark-cs4334-codec.o
obj-$(CONFIG_SND_SDDAC)					+= snd-soc-sddac.o

# ARK Machine support
snd-soc-ark-cs4270-objs 				:= ark_cs4270.o
snd-soc-ark-sddac-objs					:= ark_evk_audio.o

obj-$(CONFIG_SND_ARK_SOC_ARK_CS4270) 	+= snd-soc-ark-cs4270.o
obj-$(CONFIG_SND_ARK_SOC_ARK_SDDAC) 	+= snd-soc-ark-sddac.o

obj-$(CONFIG_FM1288_Driver)            += FM1288.o

