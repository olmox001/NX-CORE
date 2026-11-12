# NX-CORE boot project catalog

NX-CORE owns the machine bootstrap entry and the firmware-visible user-project catalog.

- `boot-entry.conf` is the single source of truth for the initial user-mode kernel extension. It currently names `/krnl/extension/nxmanageDD`.
- `user-projects.conf` is the source of truth used to generate GRUB/UEFI menu entries. Each record is `id|display title|image filename|default`.

The manager executable is common to every project. A selected project contributes its own root filesystem and protected `/system/boot/manager.conf`, `/system/boot/services.conf`, `/system/boot/project`, and `/system/boot/profile`. Service implementations are supplied independently by NX-USERKIT.

The catalog contains artifact names, not superproject source paths, so NX-CORE remains independently buildable.
