cc = gcc
target = shellex

$(target) : myshell.o csapp.o
	cc -o $(target) myshell.o csapp.o -lpthread

clean:
	rm $(target) myshell.o csapp.o