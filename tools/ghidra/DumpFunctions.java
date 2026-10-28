// Dump every function in the currently processed program as TSV:
// entry<TAB>name<TAB>body_bytes<TAB>is_thunk<TAB>calling_convention
//@category NiNoKuni
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: DumpFunctions.java OUT.tsv");
            return;
        }
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        out.println("entry\tname\tbody_bytes\tis_thunk\tcc\tinsn_len");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            Address ep = f.getEntryPoint();
            AddressSetView body = f.getBody();
            String cc = f.getCallingConventionName();
            Instruction insn = getInstructionAt(ep);
            int insnLen = insn != null ? insn.getLength() : -1;
            out.printf("%08X\t%s\t%d\t%b\t%s\t%d%n",
                Long.valueOf(ep.getOffset()),
                f.getName(),
                Long.valueOf(body.getNumAddresses()),
                Boolean.valueOf(f.isThunk()),
                cc == null ? "?" : cc,
                Integer.valueOf(insnLen));
            n++;
        }
        out.close();
        println("DumpFunctions: wrote " + n + " functions to " + args[0]);
    }
}
