PROG	= blctl
SRCS	= blctl.c
OBJS	= $(SRCS:.c=.o)
CFLAGS	+= -Wall -g

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) $(OBJS)

clean:
	rm -f $(PROG) $(OBJS)
