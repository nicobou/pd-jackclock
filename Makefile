#you may use the following:
#CFLAGS+=-I <PATH_TO_PD_SRC_DIRECTORY>

NAME = jackclock
OBJS = jackclock.o 

CFLAGS +=  -fPIC -O2 -Wall -Wimplicit -Wshadow -Wstrict-prototypes \
          -Wno-unused -Wno-parentheses -Wno-switch 

ifndef CC
 CC  = gcc
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(OBJS)
	@for i in $(NAME); do \
	echo $(NAME) ;\
	($(CC) -export_dynamic -shared -o $$i.pd_linux $$i.o -lc -lm);\
	done

clean:
	-rm -f *.o *.pd_* so_locations