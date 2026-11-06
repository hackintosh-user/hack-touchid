# Builds MetallicaIds.efi (UEFI boot service driver) with gnu-efi.
# Linux: sudo apt install gnu-efi build-essential, then: make
ARCH        = x86_64
EFIINC      = /usr/include/efi
EFILIB      = /usr/lib
CFLAGS      = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol \
              -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -fno-builtin \
              -Wall -Wextra -O2 -DGNU_EFI_USE_MS_ABI -maccumulate-outgoing-args
LDFLAGS     = -nostdlib -znocombreloc -T $(EFILIB)/elf_$(ARCH)_efi.lds -shared -Bsymbolic \
              -L$(EFILIB) $(EFILIB)/crt0-efi-$(ARCH).o

MetallicaIds.efi: MetallicaIds.so
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc \
	        --target=efi-bsdrv-$(ARCH) $< $@

MetallicaIds.so: MetallicaIds.o
	ld $(LDFLAGS) $< -o $@ -lefi -lgnuefi

MetallicaIds.o: MetallicaIds.c
	gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.so *.efi
