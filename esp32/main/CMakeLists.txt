option(PIN_TEST "Build UART loopback test" OFF)
option(PNC532_TEST "Build PNC532 test" OFF)

if(PIN_TEST)
	idf_component_register(
		SRCS "pin_test.c"
		INCLUDE_DIRS "."
		PRIV_REQUIRES driver esp_driver_uart
	)
elseif(PNC532_TEST)
	idf_component_register(
		SRCS "pn532_test.c"
		INCLUDE_DIRS "."
		PRIV_REQUIRES nvs_flash driver esp_timer
					  esp_hw_support esp_driver_uart
	)
else()
	idf_component_register(
		SRCS "main.c" "pn532.c" "relay.c"
		INCLUDE_DIRS "."
		PRIV_REQUIRES bt nvs_flash driver esp_timer
					  esp_hw_support esp_driver_uart
	)
endif()
