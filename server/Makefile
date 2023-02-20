LINK = -lev -lpthread -lcrypto -lssl
IO = ecps_server.cpp -o ecps_server.exe

debug:
	lclang++ -gdwarf-4 -Dset_Verbose=1 $(IO) $(LINK)

release:
	lclang++-size -O3 -Dset_Verbose=0 $(IO) $(LINK)
