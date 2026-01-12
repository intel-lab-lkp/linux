pkcs7-kfuzz-y := $(and $(CONFIG_KFUZZTEST),$(filter y, $(CONFIG_PKCS7_MESSAGE_PARSER)))
rsa-helper-kfuzz-y := $(and $(CONFIG_KFUZZTEST),$(filter y, $(CONFIG_CRYPTO_RSA)))
obj-$(pkcs7-kfuzz-y) += pkcs7_kfuzz.o
obj-$(rsa-helper-kfuzz-y) += rsa_helper_kfuzz.o
