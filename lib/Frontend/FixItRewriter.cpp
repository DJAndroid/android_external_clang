//===--- FixItRewriter.cpp - Fix-It Rewriter Diagnostic Client --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a diagnostic client adaptor that performs rewrites as
// suggested by code modification hints attached to diagnostics. It
// then forwards any diagnostics to the adapted diagnostic client.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/FixItRewriter.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Rewriter.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/Streams.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
#include <cstdio>
using namespace clang;

FixItRewriter::FixItRewriter(DiagnosticClient *Client, 
                             SourceManager &SourceMgr)
  : Client(Client), NumFailures(0) {
  Rewrite = new Rewriter(SourceMgr);
}

FixItRewriter::~FixItRewriter() {
  delete Rewrite;
}

bool FixItRewriter::WriteFixedFile(const std::string &InFileName, 
                                   const std::string &OutFileName) {
  if (NumFailures > 0) {
    // FIXME: Use diagnostic machinery!
    std::fprintf(stderr, 
                 "%d fix-it failures detected; code will not be modified\n",
                 NumFailures);
    return true;
  }

  llvm::OwningPtr<llvm::raw_ostream> OwnedStream;
  llvm::raw_ostream *OutFile;
  if (!OutFileName.empty()) {
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(OutFileName.c_str(), 
                                       // set binary mode (critical for Windoze)
                                       true, 
                                       Err);
    OwnedStream.reset(OutFile);
  } else if (InFileName == "-") {
    OutFile = &llvm::outs();
  } else {
    llvm::sys::Path Path(InFileName);
    std::string Suffix = Path.getSuffix();
    Path.eraseSuffix();
    Path.appendSuffix("fixit." + Suffix);
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(Path.toString().c_str(), 
                                       // set binary mode (critical for Windoze)
                                       true, 
                                       Err);
    OwnedStream.reset(OutFile);
  }  

  FileID MainFileID = Rewrite->getSourceMgr().getMainFileID();
  if (const RewriteBuffer *RewriteBuf = 
        Rewrite->getRewriteBufferFor(MainFileID)) {
    *OutFile << std::string(RewriteBuf->begin(), RewriteBuf->end());
  } else {
    std::fprintf(stderr, "Main file is unchanged\n");
  }
  OutFile->flush();

  return false;
}

bool FixItRewriter::IncludeInDiagnosticCounts() const {
  return Client? Client->IncludeInDiagnosticCounts() : true;
}

void FixItRewriter::HandleDiagnostic(Diagnostic::Level DiagLevel,
                                     const DiagnosticInfo &Info) {
  if (Client)
    Client->HandleDiagnostic(DiagLevel, Info);

  // Make sure that we can perform all of the modifications we
  // in this diagnostic.
  bool CanRewrite = Info.getNumCodeModificationHints() > 0;
  for (unsigned Idx = 0, Last = Info.getNumCodeModificationHints();
       Idx < Last; ++Idx) {
    const CodeModificationHint &Hint = Info.getCodeModificationHint(Idx);
    if (Hint.RemoveRange.isValid() &&
        Rewrite->getRangeSize(Hint.RemoveRange) == -1) {
      CanRewrite = false;
      break;
    }

    if (Hint.InsertionLoc.isValid() && 
        !Rewrite->isRewritable(Hint.InsertionLoc)) {
      CanRewrite = false;
      break;
    }
  }

  if (!CanRewrite) { 
    if (Info.getNumCodeModificationHints() > 0) {
      // FIXME: warn the user that this rewrite couldn't be done
    }

    // If this was an error, refuse to perform any rewriting.
    if (DiagLevel == Diagnostic::Error || DiagLevel == Diagnostic::Fatal) {
      if (++NumFailures == 1) {
        // FIXME: use diagnostic machinery to print this.
        std::fprintf(stderr, "error without fix-it advice detected; "
                     "fix-it will produce no output\n");
      }
    }
    return;
  }

  bool Failed = false;
  for (unsigned Idx = 0, Last = Info.getNumCodeModificationHints(); 
       Idx < Last; ++Idx) {
    const CodeModificationHint &Hint = Info.getCodeModificationHint(Idx);
    if (!Hint.RemoveRange.isValid()) {
      // We're adding code.
      if (Rewrite->InsertStrBefore(Hint.InsertionLoc, Hint.CodeToInsert))
        Failed = true;
      continue;
    }
    
    if (Hint.CodeToInsert.empty()) {
      // We're removing code.
      if (Rewrite->RemoveText(Hint.RemoveRange.getBegin(),
                              Rewrite->getRangeSize(Hint.RemoveRange)))
        Failed = true;
      continue;
    } 
      
    // We're replacing code.
    if (Rewrite->ReplaceText(Hint.RemoveRange.getBegin(),
                             Rewrite->getRangeSize(Hint.RemoveRange),
                             Hint.CodeToInsert.c_str(),
                             Hint.CodeToInsert.size()))
      Failed = true;
  }

  if (Failed) // FIXME: notify the user that the rewrite failed.
    ++NumFailures;
}
