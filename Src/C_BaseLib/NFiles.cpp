
#include <Utility.H>
#include <NFiles.H>
#include <deque>


int NFilesIter::currentDeciderIndex(-1);
Array<std::pair<int, int> > NFilesIter::unreadMessages;
int NFilesIter::minDigits(5);


NFilesIter::NFilesIter(int noutfiles, const std::string &fileprefix,
                       bool groupsets, bool setBuf)
{
  isReading     = false;
  nOutFiles     = ActualNFiles(noutfiles);
  groupSets     = groupsets;
  myProc        = ParallelDescriptor::MyProc();
  nProcs        = ParallelDescriptor::NProcs();
  nSets         = LengthOfSet(nProcs, nOutFiles);
  mySetPosition = WhichSetPosition(myProc, nProcs, nOutFiles, groupSets);
  fileNumber    = FileNumber(nOutFiles, myProc, groupSets);
  filePrefix    = fileprefix;
  fullFileName  = FileName(fileNumber, filePrefix);

  finishedWriting = false;

  if(setBuf) {
    io_buffer.resize(VisMF::GetIOBufferSize());
    fileStream.rdbuf()->pubsetbuf(io_buffer.dataPtr(), io_buffer.size());
  }

  useStaticSetSelection = true;
  coordinatorProc = ParallelDescriptor::IOProcessorNumber();
  if(myProc == coordinatorProc) {
    // ---- make a static order
    fileNumbersWriteOrder.resize(nOutFiles);
    for(int i(0); i < nProcs; ++i) {
      fileNumbersWriteOrder[FileNumber(nOutFiles, i, groupSets)].push_back(i);
    }
  }

  availableDeciders.resize(0);
  availableDeciders.reserve(nProcs);
  setZeroProcs.reserve(nOutFiles);
  for(int i(0); i < nProcs; ++i) {
    // ---- count zero set positions  and find an alternate decider
    if(NFilesIter::WhichSetPosition(i, nProcs, nOutFiles, groupSets) == 0) {
      setZeroProcs.push_back(i);
    } else {
      availableDeciders.push_back(i);
    }
  }

  if(currentDeciderIndex < 0) {
    currentDeciderIndex = nSets / 2;
    if(currentDeciderIndex >= availableDeciders.size()) {
      currentDeciderIndex = 0;
    }
  }

  bool checkNFiles(false);
  if(checkNFiles) {
    CheckNFiles(nProcs, nOutFiles, groupSets);
  }

}


void NFilesIter::SetDynamic(int deciderproc)
{
  deciderProc = deciderproc;
  // ---- we have to check currentDeciderIndex here also in case of
  // ---- different nfiles for plots and checkpoints
  if(currentDeciderIndex >= availableDeciders.size() || currentDeciderIndex < 0) {
    currentDeciderIndex = 0;
  }
  if(availableDeciders.size() > 0) {
    if(deciderProc < 0 || deciderProc >= nProcs) {
      deciderProc = availableDeciders[currentDeciderIndex];
    }
    if(NFilesIter::WhichSetPosition(deciderProc, nProcs, nOutFiles, groupSets) == 0) {
      // ---- the decider cannot have set position zero
      deciderProc = availableDeciders[currentDeciderIndex];
    }
  }
  currentDeciderIndex += nSets - 1;
  if(currentDeciderIndex >= availableDeciders.size() || currentDeciderIndex < 0) {
    currentDeciderIndex = 0;
  }
  if(myProc == deciderProc) {
    int wsp(NFilesIter::WhichSetPosition(myProc, nProcs, nOutFiles, groupSets));
#ifdef DEBUG
    std::cout << myProc << "::DDDDDDDD:  idecide:  currentDeciderIndex setpos nSets = " << currentDeciderIndex
              << "  " << wsp << "  " << nSets  << std::endl;
#endif
  }

  deciderTag = ParallelDescriptor::SeqNum();
  coordinatorTag = ParallelDescriptor::SeqNum();
  doneTag = ParallelDescriptor::SeqNum();
  writeTag = ParallelDescriptor::SeqNum();
  remainingWriters = nProcs;
  useStaticSetSelection = false;
  if(nOutFiles == nProcs) {
    useStaticSetSelection = true;
    coordinatorProc = ParallelDescriptor::IOProcessorNumber();
  } else {
    fileNumbersWriteOrder.clear();
    fileNumbersWriteOrder.resize(nOutFiles);
  }
}


NFilesIter::NFilesIter(const std::string &filename,
		       const Array<int> &readranks,
                       bool setBuf)
{
  isReading = true;
  myProc    = ParallelDescriptor::MyProc();
  nProcs    = ParallelDescriptor::NProcs();
  fullFileName = filename;
  readRanks = readranks;
  myReadIndex = indexUndefined;
  for(int i(0); i < readRanks.size(); ++i) {
    if(myProc == readRanks[i]) {
      if(myReadIndex != indexUndefined) {
        BoxLib::Abort("**** Error in NFilesIter:  readRanks not unique.");
      }
      myReadIndex = i;
    }
  }

  if(myReadIndex == indexUndefined) {  // ---- nothing to read
    finishedReading = true;
    return;
  } else {
    finishedReading = false;
  }

  if(setBuf) {
    io_buffer.resize(VisMF::GetIOBufferSize());
    fileStream.rdbuf()->pubsetbuf(io_buffer.dataPtr(), io_buffer.size());
  }

  useStaticSetSelection = true;
}


NFilesIter::~NFilesIter() {
}


bool NFilesIter::ReadyToWrite(bool appendFirst) {

#ifdef BL_USE_MPI

  if(finishedWriting) {
    return false;
  }

  if(useStaticSetSelection) {

    for(int iSet(0); iSet < nSets; ++iSet) {
      if(mySetPosition == iSet) {
        if(iSet == 0 && ! appendFirst) {   // ---- first set
          fileStream.open(fullFileName.c_str(),
                          std::ios::out | std::ios::trunc | std::ios::binary);
        } else {
          fileStream.open(fullFileName.c_str(),
                          std::ios::out | std::ios::app | std::ios::binary);
        }
        if( ! fileStream.good()) {
          BoxLib::FileOpenFailed(fullFileName);
        }
        return true;
      }

      if(mySetPosition == (iSet + 1)) {   // ---- next set waits
        int iBuff, waitForPID(-1), tag(-2);
        if(groupSets) {
          waitForPID = (myProc - nOutFiles);
          tag = (myProc % nOutFiles);
        } else {
          waitForPID = (myProc - 1);
          tag = (myProc / nSets);
        }
        ParallelDescriptor::Recv(&iBuff, 1, waitForPID, tag);
      }
    }

  } else {    // ---- use dynamic set selection

    if(mySetPosition == 0) {    // ---- return true, ready to write data

      fullFileName = BoxLib::Concatenate(filePrefix, fileNumber, minDigits);
      if(appendFirst) {
        fileStream.open(fullFileName.c_str(),
                        std::ios::out | std::ios::app | std::ios::binary);
      } else {
        fileStream.open(fullFileName.c_str(),
                        std::ios::out | std::ios::trunc | std::ios::binary);
      }
      if( ! fileStream.good()) {
        BoxLib::FileOpenFailed(fullFileName);
      }
      return true;

    } else if(myProc == deciderProc) {  // ---- this proc decides who decides

      BL_PROFILE("NFI::ReadyToWrite:decider");
      // ---- the first message received is the coordinator
      ParallelDescriptor::Recv(&coordinatorProc, 1, MPI_ANY_SOURCE, deciderTag);
#ifdef DEBUG
      std::cout << myProc << ":: coordinatorProc = " << coordinatorProc << std::endl;
#endif
      for(int i(0); i < setZeroProcs.size(); ++i) {  // ---- tell the set zero ranks  who is coordinating
        ParallelDescriptor::Send(&coordinatorProc, 1, setZeroProcs[i], coordinatorTag);
      }
      unreadMessages.push_back(std::make_pair(deciderTag, setZeroProcs.size() - 1));
    }

    // ---- these are the rest of the procs who need to write
    if( ! finishedWriting) {  // ---- the deciderProc drops through to here
      // ---- wait for signal to start writing
      ParallelDescriptor::Message rmess =
            ParallelDescriptor::Recv(&fileNumber, 1, MPI_ANY_SOURCE, writeTag);
      coordinatorProc = rmess.pid();
      fullFileName = BoxLib::Concatenate(filePrefix, fileNumber, minDigits);

      fileStream.open(fullFileName.c_str(),
                      std::ios::out | std::ios::app | std::ios::binary);
      if( ! fileStream.good()) {
        BoxLib::FileOpenFailed(fullFileName);
      }
      return true;

    }

  }
  return false;

#else
  if(finishedWriting) {
    return false;
  }
  fileStream.open(fullFileName.c_str(),
                  std::ios::out | std::ios::trunc | std::ios::binary);
  if( ! fileStream.good()) {
    BoxLib::FileOpenFailed(fullFileName);
  }
  return true;
#endif
}


bool NFilesIter::ReadyToRead() {

  if(finishedReading) {
    return false;
  }

  if(myReadIndex != 0) {    // ---- wait for rank myReadIndex - 1
    int iBuff(-1), waitForPID(readRanks[myReadIndex - 1]);
    int tag(readRanks[0]);
    ParallelDescriptor::Recv(&iBuff, 1, waitForPID, tag);
  }

  fileStream.open(fullFileName.c_str(),
                  std::ios::in | std::ios::binary);
  if( ! fileStream.good()) {
    BoxLib::FileOpenFailed(fullFileName);
  }
  return true;
}


NFilesIter &NFilesIter::operator++() {

#ifdef BL_USE_MPI

  ParallelDescriptor::Message rmess;

  if(isReading) {
    fileStream.close();

    if(myReadIndex < readRanks.size() - 1) {
      int iBuff(0), wakeUpPID(readRanks[myReadIndex + 1]);
      int tag(readRanks[0]);
      ParallelDescriptor::Send(&iBuff, 1, wakeUpPID, tag);
    }
    finishedReading = true;

  } else {  // ---- writing

    if(useStaticSetSelection) {
      fileStream.flush();
      fileStream.close();

      int iBuff(0), wakeUpPID(-1), tag(-2);
      if(groupSets) {
        wakeUpPID = (myProc + nOutFiles);
        tag = (myProc % nOutFiles);
      } else {
        wakeUpPID = (myProc + 1);
        tag = (myProc / nSets);
      }
      if(wakeUpPID < nProcs) {
        ParallelDescriptor::Send(&iBuff, 1, wakeUpPID, tag);
      }
      finishedWriting = true;

    } else {    // ---- use dynamic set selection

      if(mySetPosition == 0) {    // ---- write data

        fileStream.flush();
        fileStream.close();
        finishedWriting = true;

        // ---- tell the decider we are done
        ParallelDescriptor::Send(&myProc, 1, deciderProc, deciderTag);

        // ---- wait to find out who will coordinate
        ParallelDescriptor::Recv(&coordinatorProc, 1, deciderProc, coordinatorTag);

        if(myProc == coordinatorProc) {
          Array<std::deque<int> > procsToWrite(nOutFiles);  // ---- [fileNumber](procsToWriteToFileNumber)
          // ---- populate with the static nfiles sets
          for(int i(0); i < nProcs; ++i) {
            int procSet(WhichSetPosition(i, nProcs, nOutFiles, groupSets));
            int whichFileNumber(NFilesIter::FileNumber(nOutFiles, i, groupSets));
	    // ---- procSet == 0 have already written their data
	    if(procSet == 0) {
	      fileNumbersWriteOrder[whichFileNumber].push_back(i);
              --remainingWriters;
	    }
            if(procSet != 0) {
              procsToWrite[whichFileNumber].push_back(i);
            }
          }

          // ---- signal each remaining processor when to write and to which file
          std::set<int> availableFileNumbers;
          availableFileNumbers.insert(fileNumber);  // ---- the coordinators file number

          // ---- recv incoming available files
          while(remainingWriters > 0) {

            int nextProcToWrite, nextFileNumberToWrite, nextFileNumberAvailable;
            std::set<int>::iterator ait = availableFileNumbers.begin();
            nextFileNumberToWrite = *ait;
            availableFileNumbers.erase(nextFileNumberToWrite);

            for(int nfn(0); nfn < procsToWrite.size(); ++nfn) {
              // ---- start with the current next file number
              // ---- get a proc from another file number if the queue is empty
              int tempNFN((nextFileNumberToWrite + nfn) % procsToWrite.size());
              if(procsToWrite[tempNFN].size() > 0) {
                nextProcToWrite = procsToWrite[tempNFN].front();
                procsToWrite[tempNFN].pop_front();
                break;  // ---- found one
              }
            }

	    fileNumbersWriteOrder[nextFileNumberToWrite].push_back(nextProcToWrite);

            ParallelDescriptor::Send(&nextFileNumberToWrite, 1, nextProcToWrite, writeTag);
  
            rmess = ParallelDescriptor::Recv(&nextFileNumberAvailable, 1, MPI_ANY_SOURCE, doneTag);
            availableFileNumbers.insert(nextFileNumberAvailable);
            --remainingWriters;
          }

        } else {    // ---- tell the coordinatorProc we are done writing
          ParallelDescriptor::Send(&fileNumber, 1, coordinatorProc, doneTag);
        }

      }

      if( ! finishedWriting) {  // ---- the deciderProc drops through to here
        fileStream.flush();
        fileStream.close();
        finishedWriting = true;

        // ---- signal we are finished
        ParallelDescriptor::Send(&fileNumber, 1, coordinatorProc, doneTag);
      }

    }

  }

#else
  if(isReading) {
    fileStream.close();
    finishedReading = true;
  } else {  // ---- writing
    fileStream.flush();
    fileStream.close();
    finishedWriting = true;
  }
#endif

  return *this;
}


std::streampos NFilesIter::SeekPos() {
  return fileStream.tellp();
}


bool NFilesIter::CheckNFiles(int nProcs, int nOutFiles, bool groupSets)
{
  if(ParallelDescriptor::IOProcessor()) {
    std::set<int> fileNumbers;
    for(int i(0); i < nProcs; ++i) {
      fileNumbers.insert(FileNumber(nOutFiles, i, groupSets));
    }
    std::cout << "nOutFiles fileNumbers.size() = " << nOutFiles
              << "  " << fileNumbers.size() << std::endl;
    if(nOutFiles != fileNumbers.size()) {
      std::cout << "**** Different number of files." << std::endl;
      return false;
    }
  }
  return true;
}



Array<int> NFilesIter::FileNumbersWritten()
{
  Array<int> fileNumbersWritten(nProcs, -1);

  if(myProc == coordinatorProc) {

#ifdef DEBUG
    int total(0);
    std::set<int> procSet;
    for(int f(0); f < fileNumbersWriteOrder.size(); ++f) {
      total += fileNumbersWriteOrder[f].size();
      for(int r(0); r < fileNumbersWriteOrder[f].size(); ++r) {
        procSet.insert(fileNumbersWriteOrder[f][r]);
      }
    }
    if(total != nProcs || procSet.size() != nProcs) {
      std::cout << "**** Error in NFilesIter::FileNumbersWritten():  "
                << " coordinatorProc nProcs total procSet.size() = "
                << coordinatorProc << "  " << nProcs << "  "
		<< total << "  " << procSet.size() << std::endl;
    }
#endif

    for(int f(0); f < fileNumbersWriteOrder.size(); ++f) {
      for(int r(0); r < fileNumbersWriteOrder[f].size(); ++r) {
        fileNumbersWritten[fileNumbersWriteOrder[f][r]] = f;
      }
    }

  }
  return fileNumbersWritten;
}



void NFilesIter::CleanUpMessages() {
#ifdef BL_USE_MPI
  BL_PROFILE("NFI::CleanUpMessages");
  for(int i(0); i < unreadMessages.size(); ++i) {
    std::pair<int, int> & pii = unreadMessages[i];
    int fromProc, tag(pii.first), nMessages(pii.second);
#ifdef DEBUG
    std::cout << ParallelDescriptor::MyProc() << ":: cleaning up " << nMessages
              << " messages for tag " << tag << std::endl;
#endif
    for(int n(0); n < nMessages; ++n) {
      ParallelDescriptor::Recv(&fromProc, 1, MPI_ANY_SOURCE, tag);
    }
  }
  unreadMessages.clear();
#endif
}



