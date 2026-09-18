add_library(basis_universal STATIC
	basisu.h
	basisu_containers.h
	basisu_containers_impl.h
	basisu_file_headers.h
	basisu_transcoder.cpp
	basisu_transcoder.h
	basisu_transcoder_internal.h
	basisu_transcoder_tables_astc.inc
	basisu_transcoder_tables_astc_0_255.inc
	basisu_transcoder_tables_atc_55.inc
	basisu_transcoder_tables_atc_56.inc
	basisu_transcoder_tables_bc7_m5_alpha.inc
	basisu_transcoder_tables_bc7_m5_color.inc
	basisu_transcoder_tables_dxt1_5.inc
	basisu_transcoder_tables_dxt1_6.inc
	basisu_transcoder_tables_pvrtc2_45.inc
	basisu_transcoder_tables_pvrtc2_alpha_33.inc
	basisu_transcoder_uastc.h
)

if(USE_SYSTEM_ZSTD)
	find_path(BASIS_ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
	find_library(BASIS_ZSTD_LIBRARY NAMES zstd REQUIRED)
	target_include_directories(basis_universal PRIVATE ${BASIS_ZSTD_INCLUDE_DIR})
	target_link_libraries(basis_universal ${BASIS_ZSTD_LIBRARY})
else()
	target_link_libraries(basis_universal libzstd_static)
endif()
