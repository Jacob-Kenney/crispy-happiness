.PHONY: all lib test smoke train clean

PY ?= python3

all: lib test

lib:
	$(MAKE) -C cpp lib

test:
	$(MAKE) -C cpp test

smoke: lib
	cd python && $(PY) train_ppo.py --smoke

train: lib
	cd python && $(PY) train_ppo.py --iters 40 --eval-every 5 --save

clean:
	$(MAKE) -C cpp clean
