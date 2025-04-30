.PHONY: rpal

all: server client

server: server.c librpal/*.c librpal/*.S
	$(CC) $^ -lpthread -o $@ 
	@printf "RPAL" | dd of=./server bs=1 count=4 conv=notrunc seek=12

client: client.c librpal/*.c librpal/*.S
	$(CC) $^ -lpthread -o $@ 
	@printf "RPAL" | dd of=./client bs=1 count=4 conv=notrunc seek=12

clean:
	rm server client
