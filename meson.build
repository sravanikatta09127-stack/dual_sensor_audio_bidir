################################################
# Dual Sensor with Bidirectional Audio
################################################

dual_sensor_audio_bidir_install_dir = case_studies_install_dir + '/dual_sensor_audio_bidir'

dual_sensor_audio_bidir_app_src = ['main.cpp']

executable('dual_sensor_audio_bidir',
  dual_sensor_audio_bidir_app_src,
  cpp_args : hailo_lib_args,
  dependencies : [libhailo_analytics_dep],
  gnu_symbol_visibility : 'default',
  install: true,
  install_dir: dual_sensor_audio_bidir_install_dir,
)
