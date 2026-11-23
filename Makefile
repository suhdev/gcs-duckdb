PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=gcs_adc
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Runs the tests in test/sql/mock against a local fake of the Google OAuth2, STS, IAM, metadata and GCS endpoints
test_mock: test_mock_release
test_mock_release:
	./test/mock/run_mock_tests.sh build/release
test_mock_debug:
	./test/mock/run_mock_tests.sh build/debug
