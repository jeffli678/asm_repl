#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/param.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <setjmp.h>
#include <editline/readline.h>
#include <ctype.h>

#include "taskport_auth.h"

#include "assemble.h"
#include "colors.h"
#include "utils.h"

#include "registers.h"
#include "float_registers.h"
#include "status_flags.h"

extern boolean_t mach_exc_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

typedef union {
	uint32_t eflags;
	uint64_t rflags;
	struct __attribute__((packed)) {
		uint8_t CF    :1;
		uint8_t _res1 :1;
		uint8_t PF    :1;
		uint8_t _res2 :1;
		uint8_t AF    :1;
		uint8_t _res3 :1;
		uint8_t ZF    :1;
		uint8_t SF    :1;
		uint8_t TF    :1;
		uint8_t IF    :1;
		uint8_t DF    :1;
		uint8_t OF    :1;
		uint8_t IOPL  :2;
		uint8_t NT    :1;
		uint8_t _res4 :1;

		uint8_t RF    :1;
		uint8_t VM    :1;
		uint8_t AC    :1;
		uint8_t VIF   :1;
		uint8_t VIP   :1;
		uint8_t ID    :1;

		uint16_t _res5 :10;

		uint32_t _res6 :32;
	};
} x86_flags_t;

#if defined(__i386__)

#define BITS 32
#define ARCH_NAME "i386"

#define ts ts32
#define fs fs32

typedef uint32_t gpr_register_t;
#define REGISTER_FORMAT_DEC "%" PRIu32
#define REGISTER_FORMAT_HEX "%" PRIX32
#define REGISTER_FORMAT_HEX_PADDED "%08" PRIX32

#define pc_register __eip
#define flags_register __eflags

#define IF32(X, Y) X

#elif defined(__x86_64__)

#define BITS 64
#define ARCH_NAME "x86_64"

#define ts ts64
#define fs fs64

typedef uint64_t gpr_register_t;
#define REGISTER_FORMAT_DEC "%" PRIu64
#define REGISTER_FORMAT_HEX "%" PRIX64
#define REGISTER_FORMAT_HEX_PADDED "%016" PRIX64

#define pc_register __rip
#define flags_register __rflags

#define IF32(X, Y) Y

#else
#error Unsupported architecture
#endif

#define ISGRAPH(c) (((unsigned char)c) <= 127 && isgraph(c))

typedef union {
	_STRUCT_XMM_REG bytes;
	double doubles[2];
	float floats[4];
	uint64_t ints[2];
} xmm_value_t;

pthread_mutex_t mutex;

#define MEMORY_SIZE 0x10000
#define INT3 0xCC

#define ELEMENTS(x) (sizeof(x) / sizeof(*x))

#define LIST(x, ...)     x,
#define STR_LIST(x, ...) #x,
#define LIST2(x, y, ...) y,

#define STD_FAIL(s, x) do { \
	int ret = (x); \
	if(ret != 0) { \
		perror(s "()"); \
		exit(ret); \
	} \
} while(false)

#define KERN_FAIL(s, x) do { \
	kern_return_t ret = (x); \
	if(ret != KERN_SUCCESS) { \
		printf(s "() failed: %s\n", mach_error_string(ret)); \
		exit(ret); \
	} \
} while(false)

#define KERN_TRY(s, x, f) if(true) { \
	kern_return_t ret = (x); \
	if(ret != KERN_SUCCESS) { \
		printf(s "() failed: %s\n", mach_error_string(ret)); \
		f \
	} \
} else do {} while(0)

void get_thread_state(thread_act_t thread, x86_thread_state_t *state) {
	mach_msg_type_number_t stateCount = x86_THREAD_STATE_COUNT;
	KERN_FAIL("thread_get_state", thread_get_state(thread, x86_THREAD_STATE, (thread_state_t)state, &stateCount));
}

void set_thread_state(thread_act_t thread, x86_thread_state_t *state) {
	KERN_FAIL("thread_set_state", thread_set_state(thread, x86_THREAD_STATE, (thread_state_t)state, x86_THREAD_STATE_COUNT));
}

void get_float_state(thread_act_t thread, x86_float_state_t *state) {
	mach_msg_type_number_t stateCount = x86_FLOAT_STATE_COUNT;
	KERN_FAIL("thread_get_state", thread_get_state(thread, x86_FLOAT_STATE, (thread_state_t)state, &stateCount));
}

void set_float_state(thread_act_t thread, x86_float_state_t *state) {
	KERN_FAIL("thread_set_state", thread_set_state(thread, x86_FLOAT_STATE, (thread_state_t)state, x86_FLOAT_STATE_COUNT));
}

gpr_register_t get_pc(thread_act_t thread) {
	x86_thread_state_t state;
	get_thread_state(thread, &state);
	return state.uts.ts.pc_register;
}

void set_pc(thread_act_t thread, gpr_register_t pc_value) {
	x86_thread_state_t state;
	get_thread_state(thread, &state);
	state.uts.ts.pc_register = pc_value;
	set_thread_state(thread, &state);
}

void write_int3(task_t task, mach_vm_address_t address) {
	unsigned char int3 = INT3;
	KERN_FAIL("mach_vm_write", mach_vm_write(task, address, (vm_offset_t)&int3, sizeof(int3)));
}

void setup_child(task_t task, thread_act_t *_thread, mach_vm_address_t *_memory) {
	thread_act_array_t thread_list;
	mach_msg_type_number_t thread_count;
	KERN_FAIL("task_threads", task_threads(task, &thread_list, &thread_count));

	if(thread_count != 1) {
		printf("1 thread expected, got %d.\n", thread_count);
		exit(KERN_FAILURE);
	}

	thread_act_t thread = thread_list[0];
	*_thread = thread;

	mach_vm_address_t memory;
	KERN_FAIL("mach_vm_allocate", mach_vm_allocate(task, &memory, MEMORY_SIZE, VM_FLAGS_ANYWHERE));
	*_memory = memory;

	KERN_FAIL("mach_vm_protect", mach_vm_protect(task, memory, MEMORY_SIZE, 0, VM_PROT_ALL));

	write_int3(task, memory);

	set_pc(thread, memory);
}

// Start of the exception handler thread
void *exception_handler_main(void *arg) {
	mach_port_t exception_port = (mach_port_t)arg;
	if(mach_msg_server(mach_exc_server, 2048, exception_port, MACH_MSG_TIMEOUT_NONE) != MACH_MSG_SUCCESS) {
		puts("error: mach_msg_server()");
		exit(1);
	}

	return NULL;
}

kern_return_t  catch_mach_exception_raise_state(mach_port_t __unused exception_port, exception_type_t __unused exception, exception_data_t __unused code, mach_msg_type_number_t __unused code_count, int * __unused flavor, thread_state_t __unused in_state, mach_msg_type_number_t __unused in_state_count, thread_state_t __unused out_state, mach_msg_type_number_t * __unused out_state_count) {
	return KERN_FAILURE;
}

kern_return_t  catch_mach_exception_raise_state_identity(mach_port_t __unused exception_port, mach_port_t __unused thread, mach_port_t __unused task, exception_type_t __unused exception, exception_data_t __unused code, mach_msg_type_number_t __unused code_count, int * __unused flavor, thread_state_t __unused in_state, mach_msg_type_number_t __unused in_state_count, thread_state_t __unused out_state, mach_msg_type_number_t * __unused out_state_count) {
	return KERN_FAILURE;
}

// Called when an exception is caught from the child, e.g. SIGTRAP
kern_return_t catch_mach_exception_raise(mach_port_t __unused exception_port, mach_port_t thread, mach_port_t __unused task, exception_type_t exception, exception_data_t __unused code, mach_msg_type_number_t __unused code_count) {
	if(exception == EXC_BREAKPOINT) {
		KERN_FAIL("task_suspend", task_suspend(task));
		set_pc(thread, get_pc(thread) - 1);
		pthread_mutex_unlock(&mutex);
		return KERN_SUCCESS;
	} else {
		return KERN_FAILURE;
	}
}

void setup_exception_handler(task_t task) {
	mach_port_t exception_port;
	KERN_FAIL("mach_port_allocate", mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exception_port));
	KERN_FAIL("mach_port_insert_right", mach_port_insert_right(mach_task_self(), exception_port, exception_port, MACH_MSG_TYPE_MAKE_SEND));
	KERN_FAIL("task_set_exception_port", task_set_exception_ports(task, EXC_MASK_BREAKPOINT, exception_port, (exception_behavior_t)(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES), MACHINE_THREAD_STATE));

	pthread_t exception_handler_thread;
	STD_FAIL("pthread_create", pthread_create(&exception_handler_thread, NULL, exception_handler_main, (void *)(uintptr_t)exception_port));
}

#define FOREACH_TYPE(X) \
	X(gpr, true) \
	X(status, true) \
	X(fpr_hex, false) \
	X(fpr_double, false)

typedef enum {
	FOREACH_TYPE(LIST)
} register_type;

char *register_type_names[] = {
	FOREACH_TYPE(STR_LIST)
};

bool show_register_types[] = {
	FOREACH_TYPE(LIST2)
};

void print_registers(x86_thread_state_t *state, x86_float_state_t *float_state) {
	puts("");

	static x86_thread_state_t last_state;
	static x86_float_state_t last_float_state;
	static x86_flags_t last_flags;
	static bool first = true;

	if(show_register_types[fpr_double]) {
#define X(r) do { \
	xmm_value_t v = (xmm_value_t)float_state->ufs.fs.__fpu_ ## r; \
	xmm_value_t l = (xmm_value_t)last_float_state.ufs.fs.__fpu_ ## r; \
	bool c1 = !first && v.ints[0] != l.ints[0]; \
	bool c2 = !first && v.ints[1] != l.ints[1]; \
	printf(KGRN "%" IF32("4", "5") "s:" RESET " { %s%e" RESET ", %s%e" RESET " }\n", #r, c1? KRED: RESET, v.doubles[0], c2? KRED: RESET, v.doubles[1]); \
} while(false)
	FOREACH_FLOAT_REGISTER(X)
#undef X
	}

	if(show_register_types[fpr_hex]) {
#define X(r) do { \
	xmm_value_t v = (xmm_value_t)float_state->ufs.fs.__fpu_ ## r; \
	xmm_value_t l = (xmm_value_t)last_float_state.ufs.fs.__fpu_ ## r; \
	bool c = !first && (v.ints[0] != l.ints[0] || v.ints[1] != l.ints[1]); \
	printf(KGRN "%" IF32("4", "5") "s: %s%016" PRIX64 "%016" PRIX64 RESET "\n", #r, c? KRED: RESET, v.ints[0], v.ints[1]); \
} while(false)
	FOREACH_FLOAT_REGISTER(X)
#undef X
	}

	if(show_register_types[gpr]) {
		int i = 0;
		int columns = IF32(4, 3);
#define X(r) do { \
	gpr_register_t v = state->uts.ts.__ ## r; \
	bool c = !first && v != last_state.uts.ts.__ ## r; \
	printf(KGRN "%3s: %s" REGISTER_FORMAT_HEX_PADDED RESET "%s", #r, c? KRED: RESET, v, (i % columns == columns - 1 || i == REGISTERS - 1)? "\n": "  "); \
	i++; \
} while(false)
	FOREACH_REGISTER(X)
#undef X
	}

	x86_flags_t flags = (x86_flags_t)state->uts.ts.flags_register;

	if(show_register_types[status]) {
		printf(KBLU "Status:" KNRM);

#define X(f) do { \
	uint8_t v = flags.f; \
	bool c = !first && v != last_flags.f; \
	printf("  " KGRN "%s: %s%d" RESET, #f, c? KRED: RESET, v); \
} while(false)
	FOREACH_STATUS_FLAG(X)
#undef X
	}

	puts("");

	first = false;
	last_state = *state;
	last_float_state = *float_state;
	last_flags = flags;
}

gpr_register_t *get_gpr_pointer(char *name, x86_thread_state_t *state) {
#define X(r) do { \
	if(strcmp(name, #r) == 0) { \
		return &(state->uts.ts.__ ## r); \
	} \
} while(false)
	FOREACH_REGISTER(X)
#undef X

	return NULL;
}

xmm_value_t *get_fpr_pointer(char *name, x86_float_state_t *float_state) {
#define X(r) do { \
	if(strcmp(name, #r) == 0) { \
		return (xmm_value_t *)&(float_state->ufs.fs.__fpu_ ## r); \
	} \
} while(false)
	FOREACH_FLOAT_REGISTER(X)
#undef X

	return NULL;
}

bool get_number(char *str, gpr_register_t *val) {
	char *endptr;
	*val = strtoll(str, &endptr, 0);
	return *endptr == '\0';
}

bool get_value(char *str, x86_thread_state_t *state, gpr_register_t *val) {
	if(get_number(str, val)) {
		return true;
	}

	gpr_register_t *gpr = get_gpr_pointer(str, state);
	if(gpr) {
		*val = *gpr;
		return true;
	}

	return false;
}

size_t count_tokens(char *str, char *seperators) {
	size_t i = 0;
	char *p = strdup(str);
	while(strsep(&p, seperators)) {
		i++;
	}
	free(p);
	return i;
}

char *histfile;
bool waiting_for_input = false;
jmp_buf prompt_jmp_buf;

int syntax_type = 0; // 0 = intel, 1 = at&t

void read_input(task_t task, thread_act_t thread, x86_thread_state_t *state, x86_float_state_t *float_state) {
	static char *line = NULL;
	while(true) {
		if(line) {
			free(line);
		}

		waiting_for_input = true;
		setjmp(prompt_jmp_buf);

		line = readline("> ");

		waiting_for_input = false;

		if(!line) {
			exit(0);
		}

		if(line[0] == '\0') {
			continue;
		}

		add_history(line);
		write_history(histfile);

#define FOREACH_CMD(X) \
	X(set) \
	X(read) \
	X(write) \
	X(writestr) \
	X(alloc) \
	X(regs) \
	X(show) \
	X(syntax)
		typedef enum {
			FOREACH_CMD(LIST)
		} cmds;
		static char *cmd_names[] = {
			FOREACH_CMD(STR_LIST)
		};

		static char *help[] = {
			"Usage: .set register value\n"
			"Changes the value of a register\n"
			"\n"
			"  register - register name (GPR, FPR or status)\n"
			"  value    - hex if GPR or FPR, 0 or 1 if status",

			"Usage: .read address [len]\n"
			"Displays a hexdump of memory starting at address\n"
			"\n"
			"  address - an integer or a register name\n"
			"  len     - the amount of bytes to read",

			"Usage: .write address hexpairs\n"
			"Writes hexpairs to a destination address\n"
			"\n"
			"  address  - an integer or a register name\n"
			"  hexpairs - pairs of hexadecimal numbers",

			"Usage: .writestr address string\n"
			"Writes an ascii string to a destination address\n"
			"\n"
			"  address - an integer or a register name\n"
			"  string  - an ascii string",

			"Usage: .alloc len\n"
			"Allocates some memory and returns the address\n"
			"\n"
			"  len - the amount of bytes to allocate",

			"Usage: .regs\n"
			"Displays the values of the registers currently toggled on",

			"Usage: .show [gpr|status|fpr_hex|fpr_double]\n"
			"Toggles which types of registers are shown\n"
			"\n"
			"  gpr        - General purpose registers (rax, rsp, rip, ...)\n"
			"  status     - Status registers (CF, ZF, ...)\n"
			"  fpr_hex    - Floating point registers shown in hex (xmm0, xmm1, ...)\n"
			"  fpr_double - Floating point registers shown as doubles",

			"Usage: .syntax [att|intel]\n"
			"Changes the assembly syntax to intel or at&t\n"
		};

		ssize_t cmd = -1;
		if(line[0] == '?' || line[0] == '.') {
			for(size_t i = 0; i != ELEMENTS(cmd_names); i++) {
				size_t len = strlen(cmd_names[i]);
				if(strncmp(cmd_names[i], line + 1, len) == 0 && (line[len + 1] == '\0' || line[len + 1] == ' ')) {
					cmd = i;
					break;
				}
			}
		}

		if(line[0] == '?') {
			if(cmd != -1) {
				puts(help[cmd]);
				continue;
			}

			puts("Valid input:\n"
			       "  Help:\n"
			       "    ?      - show this help\n"
				   "    ?[cmd] - show help for a command\n"
				   "\n"
				   "  Commands:\n"
				   "    .set      - change value of register\n"
				   "    .read     - read from memory\n"
				   "    .write    - write hex to memory\n"
				   "    .writestr - write string to memory\n"
				   "    .alloc    - allocate memory\n"
				   "    .regs     - show the contents of the registers\n"
				   "    .show     - toggle shown register types\n"
				   "    .syntax   - change the assembly syntax to intel or at&t\n"
				   "\n"
				   "Any other input will be interpreted as " ARCH_NAME " assembly"
			);
		} else if(line[0] == '.') {
			size_t args = count_tokens(line, " ") - 1;

			char *p = line + 1;
			char *cmd_name = strsep(&p, " ");
			char *arg1 = strsep(&p, " ");
			char *arg2 = strsep(&p, " ");

			switch(cmd) {
				case set: {
					if(args != 2) {
						puts(help[cmd]);
						continue;
					}

					size_t len = strlen(arg2);
					if(len == 1) {
						char c = arg2[0];
						if(c == '0' || c == '1') {
							x86_flags_t *flags = (x86_flags_t *)&state->uts.ts.flags_register;
							bool matched = false;
#define X(f) do { \
	if(strcmp(arg1, #f) == 0) { \
		flags->f = c - '0'; \
		matched = true; \
	} \
} while(false)
	FOREACH_STATUS_FLAG(X)
#undef X

							if(matched) {
								continue;
							}
						}
					}

					size_t size;
					unsigned char *data = hex2bytes(arg2, &size, true);
					if(!data) {
						puts(help[cmd]);
						continue;
					}

					size_t expected_size;
					gpr_register_t *gpr = get_gpr_pointer(arg1, state);
					xmm_value_t *xmm;
					if(gpr) {
						expected_size = sizeof(*gpr);
					} else {
						xmm = get_fpr_pointer(arg1, float_state);
						if(xmm) {
							expected_size = sizeof(*xmm);
						}
					}

					if((!gpr && !xmm) || expected_size < size) {
						puts(help[cmd]);
						free(data);
						continue;
					}

					if(gpr) {
						*gpr = 0;
						unsigned char *ptr = (void *)gpr;
						for(size_t i = 0; i != size; i++) {
							ptr[i] = data[size - i - 1];
						}
						set_thread_state(thread, state);
					} else {
						xmm->ints[0] = 0;
						xmm->ints[1] = 0;
						unsigned char *p1 = (void *)&(xmm->ints[1]);
						unsigned char *p2 = (void *)&(xmm->ints[0]);
						for(size_t i = 0; i != size; i++) {
							if(i < sizeof(*xmm->ints)) {
								p1[i] = data[size - i - 1];
							} else {
								p2[i % sizeof(*xmm->ints)] = data[size - i - 1];
							}
						}
						set_float_state(thread, float_state);
					}

					free(data);

					break;
				}
				case read: {
					gpr_register_t address;
					if(args < 1 || args > 2 || !get_value(arg1, state, &address)) {
						puts(help[cmd]);
						continue;
					}

					gpr_register_t len = 0x20;
					if(args == 2) {
						if(!get_number(arg2, &len)) {
							puts(help[cmd]);
							continue;
						}
					}

					unsigned char *data = malloc(len);
					mach_vm_size_t count;
					KERN_TRY("mach_vm_read_overwrite", mach_vm_read_overwrite(task, address, len, (mach_vm_address_t)data, &count), {
						free(data);
						continue;
					});

					const size_t row_bytes = 8;
					for(int i = 0; i < count; i += row_bytes) {
						char str[3 * row_bytes + 2 + row_bytes];
						for(int j = 0; j < row_bytes && i + j < count; j++) {
							unsigned char c = data[i + j];
							str[3 * j] = int2hex(c >> 4);
							str[3 * j + 1] = int2hex(c & 0x0f);
							str[3 * j + 2] = ' ';
							str[3 * row_bytes + 1 + j] = ISGRAPH(c)? c: '.';
						}
						str[3 * row_bytes] = ' ';
						str[sizeof(str) - 1] = '\0';
						printf(REGISTER_FORMAT_HEX ": %s\n", address + i, str);
					}

					free(data);
					break;
				}
				case write: {
					gpr_register_t address;
					if(args != 2 || !get_value(arg1, state, &address)) {
						puts(help[cmd]);
						continue;
					}

					size_t size;
					unsigned char *data = hex2bytes(arg2, &size, false);
					if(!data) {
						printf("Invalid hexpairs!\n");
						continue;
					}

					KERN_TRY("mach_vm_write", mach_vm_write(task, address, (vm_offset_t)data, size), {
						free(data);
						continue;
					});

					printf("Wrote %zu bytes.\n", size);

					free(data);
					break;
				}
				case writestr: {
					gpr_register_t address;
					if(args != 2 || !get_value(arg1, state, &address)) {
						puts(help[cmd]);
						continue;
					}

					size_t size = strlen(arg2) + 1;

					KERN_TRY("mach_vm_write", mach_vm_write(task, address, (vm_offset_t)arg2, size), {
						continue;
					});

					printf("Wrote %zu bytes.\n", size);

					break;
				}
				case alloc: {
					gpr_register_t size;
					if(args != 1 || !get_number(arg1, &size)) {
						puts(help[cmd]);
						continue;
					}

					mach_vm_address_t address;
					KERN_TRY("mach_vm_allocate", mach_vm_allocate(task, &address, size, VM_FLAGS_ANYWHERE), {
						continue;
					});

					printf("Allocated " REGISTER_FORMAT_DEC " bytes at 0x%llx\n", size, address);
					break;
				}
				case regs: {
					print_registers(state, float_state);
					break;
				}
				case show: {
					if(args == 1) {
						bool toggled = false;
						for(size_t i = 0; i < ELEMENTS(register_type_names); i++) {
							if(strcmp(arg1, register_type_names[i]) == 0) {
								bool val = !show_register_types[i];
								show_register_types[i] = val;
								printf("%s toggled %s\n", arg1, val? "on": "off");
								toggled = true;
								break;
							}
						}
						if(toggled) {
							continue;
						}
					}

					puts(help[cmd]);
					break;
				}
				case syntax: {
					if(args == 1) {
						int type = -1;
						if(strcmp(arg1, "intel") == 0) {
							type = 0;
						}
						if(strcmp(arg1, "att") == 0) {
							type = 1;
						}

						if(type != -1) {
							syntax_type = type;
							continue;
						}
					}

					if(args == 0) {
						printf("Current syntax: %s\n", syntax_type? "att": "intel");
					}

					puts(help[cmd]);
					break;
				}
				default: {
					printf("Invalid command: .%s\n", cmd_name);
					break;
				}
			}
		} else {
			unsigned char *assembly;
			size_t asm_len;
			mach_vm_address_t pc = state->uts.ts.pc_register;
			if(assemble_string(line, BITS, pc, &assembly, &asm_len, syntax_type)) {
				KERN_FAIL("mach_vm_write", mach_vm_write(task, pc, (vm_offset_t)assembly, asm_len));
				free(assembly);
				write_int3(task, pc + asm_len);
				break;
			} else {
				puts("Failed to assemble instruction.");
			}
		}
	}
}

void setup_readline() {
	// Disable file auto-complete
	rl_bind_key('\t', rl_insert);

	asprintf(&histfile, "%s/%s", getenv("HOME"), ".asm_repl_history");
	read_history(histfile);
}

#define READY 'R'

void write_ready(int fd) {
	static char ready = READY;
	write(fd, &ready, sizeof(ready));
}

void read_ready(int fd) {
	char buf;
	if(read(fd, &buf, sizeof(buf)) <= 0 || buf != READY) {
		puts("Failed to read");
		exit(1);
	}
}

task_t child_task;

void sigint_handler(int sig) {
	if(waiting_for_input) {
		// Clear line
		printf("\33[2K\r");
		// Print prompt again
		longjmp(prompt_jmp_buf, 0);
	} else {
		// Suspend child and prompt for input
		puts("");
		task_suspend(child_task);
		pthread_mutex_unlock(&mutex);
	}
}

void sigchld_handler(int sig) {
	int status;
	waitpid(-1, &status, WNOHANG);
	if(WIFSIGNALED(status)) {
		puts("Process died!");
		exit(1);
	}
}

int main(int argc, const char *argv[]) {
	if(!taskport_auth()) {
		puts("Failed to get taskport auth!");
		exit(1);
	}

	int p1[2];
	int p2[2];
	pipe(p1);
	pipe(p2);

	int parent_read = p1[0];
	int child_write = p1[1];
	int child_read = p2[0];
	int parent_write = p2[1];

	pid_t pid = fork();
	if(pid == -1) {
		perror("fork");
		return 1;
	}

	if(pid == 0) {
		close(parent_read);
		close(parent_write);

		signal(SIGINT, SIG_IGN);

		// Try to drop privileges
		setgid(-2);
		setuid(-2);

		// We are ready for the parent to register the exception handlers
		write_ready(child_write);

		// Wait for the parents exception handler
		read_ready(child_read);

		// This will be caught by the parents exception handler
		__asm__("int3");
	} else {
		close(child_read);
		close(child_write);

		signal(SIGINT, sigint_handler);
		signal(SIGCHLD, sigchld_handler);

		setup_readline();

		// Wait for the child to be ready
		read_ready(parent_read);

		task_t task;
		if(task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
			puts("task_for_pid() failed!");
			puts("Either codesign asm_repl or run as root.");
			exit(1);
		}
		child_task = task;

		pthread_mutex_init(&mutex, NULL);
		pthread_mutex_lock(&mutex);

		setup_exception_handler(task);

		// We have set up the exception handler so we make the child raise SIGTRAP
		write_ready(parent_write);

		// Wait for exception handler to be called
		pthread_mutex_lock(&mutex);

		thread_act_t thread;
		mach_vm_address_t memory;
		setup_child(task, &thread, &memory);

		task_resume(task);

		while(true) {
			// Wait for exception handler
			pthread_mutex_lock(&mutex);

			x86_thread_state_t state;
			get_thread_state(thread, &state);

			x86_float_state_t float_state;
			get_float_state(thread, &float_state);

			print_registers(&state, &float_state);

			read_input(task, thread, &state, &float_state);

			task_resume(task);
		}
	}

	return 0;
}
