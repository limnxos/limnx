# Add to QEMU's hw/virtio/meson.build:
#
#   specific_ss.add(when: 'CONFIG_VIRTIO_ACCEL', if_true: files('virtio-accel.c'))
#
# And to hw/virtio/Kconfig:
#
#   config VIRTIO_ACCEL
#       bool
#       default y
#       depends on VIRTIO

specific_ss.add(when: 'CONFIG_VIRTIO_ACCEL', if_true: files('virtio-accel.c'))
