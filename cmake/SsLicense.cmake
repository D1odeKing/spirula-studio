# ss_license: which model licences the user has accepted, and their full texts.
#
# A leaf like ss_i18n. The GUI's consent dialog, the CLI's prompt and any model
# that gates a download link it; the inference layer does not (nn/io/Fetch.h
# takes the gate as a callback). Families beyond the built-in four register
# themselves with license::register_terms().

add_library(ss_license STATIC
    ${SS_SRC}/core/LicenseConsent.cpp
    ${SS_SRC}/core/LicenseTexts.cpp)
target_include_directories(ss_license PUBLIC ${SS_SRC})
target_compile_options(ss_license PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${SPLAT_CXX_FLAGS}>)
set_property(TARGET ss_license PROPERTY CXX_STANDARD 17)
