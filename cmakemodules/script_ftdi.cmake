# Check for the FTDI headers (Linux only, in win32
#  we use built-in header & dynamic DLL load):
# ===================================================
IF(UNIX)
	FIND_FILE(FTDI_CONFIG_FILE libftdi-config)
	IF(FTDI_CONFIG_FILE)
		MARK_AS_ADVANCED(FTDI_CONFIG_FILE)

		SET(CMAKE_MRPT_HAS_FTDI 1)
		SET(CMAKE_MRPT_HAS_FTDI_SYSTEM 1)

		# Get the config params:
		EXECUTE_PROCESS(COMMAND ${FTDI_CONFIG_FILE} --libs
			RESULT_VARIABLE CMAKE_FTDI_CONFIG_RES
			OUTPUT_VARIABLE CMAKE_FTDI_LIBS
			OUTPUT_STRIP_TRAILING_WHITESPACE
			)
		IF(${CMAKE_FTDI_CONFIG_RES})
			MESSAGE("Error invoking FTDI config file:\n ${FTDI_CONFIG_FILE} --libs")
		ENDIF(${CMAKE_FTDI_CONFIG_RES})

		EXECUTE_PROCESS(COMMAND ${FTDI_CONFIG_FILE} --cflags
			RESULT_VARIABLE CMAKE_FTDI_CONFIG_RES
			OUTPUT_VARIABLE CMAKE_FTDI_CFLAGS
			OUTPUT_STRIP_TRAILING_WHITESPACE
			)
		IF(${CMAKE_FTDI_CONFIG_RES})
			MESSAGE("Error invoking FTDI config file:\n ${FTDI_CONFIG_FILE} --cflags")
		ENDIF(${CMAKE_FTDI_CONFIG_RES})

		ADD_DEFINITIONS(${CMAKE_FTDI_CFLAGS})

	ELSE(FTDI_CONFIG_FILE)
		SET(CMAKE_MRPT_HAS_FTDI 0)
	ENDIF(FTDI_CONFIG_FILE)
	# This option will be available only on Linux, hence it's declared here:
	OPTION(DISABLE_FTDI "Do not use the USB driver for FTDI chips" 0)
	MARK_AS_ADVANCED(DISABLE_FTDI)
ELSE(UNIX)
	# In windows we always have FTDI support (at compile time at least...)
	SET(CMAKE_MRPT_HAS_FTDI 1)
ENDIF(UNIX)

IF(NOT MSVC)
	IF(CMAKE_MRPT_HAS_FTDI)
		IF (UNIX)
			# ${CMAKE_FTDI_LIBS}
			APPEND_MRPT_LIBS(ftdi usb)
		ELSE(UNIX)
			# Nothing...
		ENDIF(UNIX)
	ENDIF(CMAKE_MRPT_HAS_FTDI)
ENDIF(NOT MSVC)