# SPDX-License-Identifier: Apache-2.0
# pinmux@41008000 and gpio@41008000 share an address on SAM0.
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
