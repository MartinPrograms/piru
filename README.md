# PIRU Kernel

A modern hybrid monolithic kernel

## Architecture

2 phase initialization
- `Bootstrap` - is a fixed, handwritten function that is required to function fully 
- `InitializeSubsystems` - uses reflection to find subsystems, and loads them. subsystems are declared using annotations, and their order of initialization is sorted during compile time. Preventing cyclical dependencies, before it boots
- `Start` - brings up scheduler, and spawns `/init` and then hands off to userspace

## Reflection usage
- Subsystem dependency registry and init ordering
- Kernel introspection, for debugging during runtime
- NOT extensively used during bootstrap, since it needs to be predictable

## Syscall ABI
- Primary interface is submission/completion ring buffers (io_uring like), set up via one syscall, then userspace batches requests without a syscall per operation
- It is possible to do synchronous execution of syscalls, but this is secondary 

Each syscall is represented by 6 values
```c
struct syscall_raw {
    uint64_t values[6];
} 
```

When adding one asynchronously, you add it to a ring buffer, which flushes when enough time and or syscalls get added to it. 
However if you want to run a syscall immediately, you can do that by running the synchronous one.

## Userspace
- Custom C runtime (crt0 and wrappers over syscalls)
- `/init` process

## Drivers
- Basic display driver
- Audio driver
- USB HID (keyboard/mouse)

All automatically linked into the kernel memory

## Userspace programs
- Shell program
