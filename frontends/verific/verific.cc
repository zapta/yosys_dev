/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

#ifdef VERIFIC_DIR

#include "veri_file.h"
#include "vhdl_file.h"
#include "VeriWrite.h"
#include "DataBase.h"
#include "Message.h"

#ifdef VERIFIC_NAMESPACE
using namespace Verific ;
#endif

static void msg_func(msg_type_t msg_type, const char *message_id, linefile_type linefile, const char *msg, va_list args)
{
	log("VERIFIC-%s [%s] ",
			msg_type == VERIFIC_NONE ? "NONE" :
			msg_type == VERIFIC_ERROR ? "ERROR" :
			msg_type == VERIFIC_WARNING ? "WARNING" :
			msg_type == VERIFIC_IGNORE ? "IGNORE" :
			msg_type == VERIFIC_INFO ? "INFO" :
			msg_type == VERIFIC_COMMENT ? "COMMENT" :
			msg_type == VERIFIC_PROGRAM_ERROR ? "PROGRAM_ERROR" : "UNKNOWN", message_id);
	if (linefile)
		log("%s:%d: ", LineFile::GetFileName(linefile), LineFile::GetLineNo(linefile));
	logv(msg, args);
	log("\n");
}

void import_attributes(std::map<RTLIL::IdString, RTLIL::Const> &attributes, DesignObj *obj)
{
	MapIter mi;
	Att *attr;

	if (obj->Linefile())
		attributes["\\src"] = stringf("%s:%d", LineFile::GetFileName(obj->Linefile()), LineFile::GetLineNo(obj->Linefile()));

	// FIXME: Parse numeric attributes
	FOREACH_ATTRIBUTE(obj, mi, attr)
		attributes[RTLIL::escape_id(attr->Key())] = RTLIL::Const(std::string(attr->Value()));
}

static void import_netlist(RTLIL::Design *design, Netlist *nl, std::set<Netlist*> &nl_todo)
{
	if (design->modules.count(RTLIL::escape_id(nl->Owner()->Name())))
		log_cmd_error("Re-definition of module `%s'.\n", nl->Owner()->Name());
	
	RTLIL::Module *module = new RTLIL::Module;
	module->name = RTLIL::escape_id(nl->Owner()->Name());
	design->modules[module->name] = module;

	log("Importing module %s.\n", RTLIL::id2cstr(module->name));

	std::map<Net*, RTLIL::SigBit> net_map;

	MapIter mi, mi2;
	Port *port;
	PortBus *portbus;
	Net *net;
	NetBus *netbus;
	Instance *inst;

	FOREACH_PORT_OF_NETLIST(nl, mi, port)
	{
		if (port->Bus())
			continue;

		// log("  importing port %s.\n", port->Name());

		RTLIL::Wire *wire = new RTLIL::Wire;
		wire->name = RTLIL::escape_id(port->Name());
		import_attributes(wire->attributes, port);
		module->add(wire);

		if (port->GetDir() == DIR_INOUT || port->GetDir() == DIR_IN)
			wire->port_input = true;
		if (port->GetDir() == DIR_INOUT || port->GetDir() == DIR_OUT)
			wire->port_output = true;

		if (port->GetNet()) {
			net = port->GetNet();
			if (net_map.count(net) == 0)
				net_map[net] = wire;
			else if (wire->port_input)
				module->connections.push_back(RTLIL::SigSig(net_map.at(net), wire));
			else
				module->connections.push_back(RTLIL::SigSig(wire, net_map.at(net)));
		}
	}

	FOREACH_PORTBUS_OF_NETLIST(nl, mi, portbus)
	{
		// log("  importing portbus %s.\n", portbus->Name());

		RTLIL::Wire *wire = new RTLIL::Wire;
		wire->name = RTLIL::escape_id(portbus->Name());
		wire->width = portbus->Size();
		wire->start_offset = std::min(portbus->LeftIndex(), portbus->RightIndex());
		import_attributes(wire->attributes, port);
		module->add(wire);

		if (portbus->GetDir() == DIR_INOUT || portbus->GetDir() == DIR_IN)
			wire->port_input = true;
		if (portbus->GetDir() == DIR_INOUT || portbus->GetDir() == DIR_OUT)
			wire->port_output = true;

		for (int i = portbus->LeftIndex();; i += portbus->IsUp() ? +1 : -1) {
			if (portbus->ElementAtIndex(i) && portbus->ElementAtIndex(i)->GetNet()) {
				net = portbus->ElementAtIndex(i)->GetNet();
				RTLIL::SigBit bit(wire, i - wire->start_offset);
				if (net_map.count(net) == 0)
					net_map[net] = bit;
				else if (wire->port_input)
					module->connections.push_back(RTLIL::SigSig(net_map.at(net), bit));
				else
					module->connections.push_back(RTLIL::SigSig(bit, net_map.at(net)));
			}
			if (i == portbus->RightIndex())
				break;
		}
	}

	module->fixup_ports();

	FOREACH_NET_OF_NETLIST(nl, mi, net)
	{
		if (net_map.count(net)) {
			// log("  skipping net %s.\n", net->Name());
			continue;
		}

		if (net->Bus())
			continue;

		// log("  importing net %s.\n", net->Name());

		RTLIL::Wire *wire = new RTLIL::Wire;
		wire->name = RTLIL::escape_id(net->Name());
		while (module->count_id(wire->name))
			wire->name += "_";
		import_attributes(wire->attributes, port);
		module->add(wire);

		if (net_map.count(net) == 0)
			net_map[net] = wire;
		else
			module->connections.push_back(RTLIL::SigSig(wire, net_map.at(net)));
	}

	FOREACH_NETBUS_OF_NETLIST(nl, mi, netbus)
	{
		bool found_new_net = false;
		for (int i = netbus->LeftIndex();; i += netbus->IsUp() ? +1 : -1) {
			net = netbus->ElementAtIndex(i);
			if (net_map.count(net) == 0)
				found_new_net = true;
			if (i == netbus->RightIndex())
				break;
		}

		if (found_new_net)
		{
			// log("  importing netbus %s.\n", netbus->Name());

			RTLIL::Wire *wire = new RTLIL::Wire;
			wire->name = RTLIL::escape_id(netbus->Name());
			wire->width = netbus->Size();
			wire->start_offset = std::min(netbus->LeftIndex(), netbus->RightIndex());
			while (module->count_id(wire->name))
				wire->name += "_";
			import_attributes(wire->attributes, port);
			module->add(wire);

			for (int i = netbus->LeftIndex();; i += netbus->IsUp() ? +1 : -1) {
				if (netbus->ElementAtIndex(i)) {
					net = netbus->ElementAtIndex(i);
					RTLIL::SigBit bit(wire, i - wire->start_offset);
					if (net_map.count(net) == 0)
						net_map[net] = bit;
					else
						module->connections.push_back(RTLIL::SigSig(bit, net_map.at(net)));
				}
				if (i == netbus->RightIndex())
					break;
			}
		}
		else
		{
			// log("  skipping netbus %s.\n", netbus->Name());
		}
	}

	FOREACH_INSTANCE_OF_NETLIST(nl, mi, inst)
	{
		log("  importing cell %s (%s).\n", inst->Name(), inst->View()->Owner()->Name());

		if (inst->Type() == PRIM_PWR) {
			module->connections.push_back(RTLIL::SigSig(net_map.at(inst->GetOutput()), RTLIL::State::S1));
			continue;
		}

		if (inst->Type() == PRIM_GND) {
			module->connections.push_back(RTLIL::SigSig(net_map.at(inst->GetOutput()), RTLIL::State::S0));
			continue;
		}

		if (inst->Type() == PRIM_X) {
			module->connections.push_back(RTLIL::SigSig(net_map.at(inst->GetOutput()), RTLIL::State::Sx));
			continue;
		}

		if (inst->Type() == PRIM_Z) {
			module->connections.push_back(RTLIL::SigSig(net_map.at(inst->GetOutput()), RTLIL::State::Sz));
			continue;
		}

		if (inst->Type() == PRIM_AND || inst->Type() == PRIM_OR || inst->Type() == PRIM_XOR || inst->Type() == PRIM_XNOR) {
			RTLIL::Cell *cell = new RTLIL::Cell;
			cell->name = RTLIL::escape_id(inst->Name());
			cell->type = inst->Type() == PRIM_AND ? "$and" : inst->Type() == PRIM_OR ? "$or" :
					inst->Type() == PRIM_XOR ? "$xor" : "$xnor";
			cell->parameters["\\A_SIGNED"] = 0;
			cell->parameters["\\B_SIGNED"] = 0;
			cell->parameters["\\A_WIDTH"] = 1;
			cell->parameters["\\B_WIDTH"] = 1;
			cell->parameters["\\Y_WIDTH"] = 1;
			cell->connections["\\A"] = net_map.at(inst->GetInput1());
			cell->connections["\\B"] = net_map.at(inst->GetInput2());
			cell->connections["\\Y"] = net_map.at(inst->GetOutput());
			module->add(cell);
			continue;
		}

		if (inst->Type() == PRIM_INV) {
			RTLIL::Cell *cell = new RTLIL::Cell;
			cell->name = RTLIL::escape_id(inst->Name());
			cell->type = "$not";
			cell->parameters["\\A_SIGNED"] = 0;
			cell->parameters["\\A_WIDTH"] = 1;
			cell->parameters["\\Y_WIDTH"] = 1;
			cell->connections["\\A"] = net_map.at(inst->GetInput());
			cell->connections["\\Y"] = net_map.at(inst->GetOutput());
			module->add(cell);
			continue;
		}

		if (inst->Type() == PRIM_MUX) {
			RTLIL::Cell *cell = new RTLIL::Cell;
			cell->name = RTLIL::escape_id(inst->Name());
			cell->type = "$mux";
			cell->parameters["\\WIDTH"] = 1;
			cell->connections["\\A"] = net_map.at(inst->GetInput1());
			cell->connections["\\B"] = net_map.at(inst->GetInput2());
			cell->connections["\\S"] = net_map.at(inst->GetControl());
			cell->connections["\\Y"] = net_map.at(inst->GetOutput());
			module->add(cell);
			continue;
		}

		if (inst->Type() == PRIM_FADD)
		{
			RTLIL::Cell *cell1 = new RTLIL::Cell;
			cell1->name = RTLIL::escape_id(NEW_ID);
			cell1->type = "$add";
			cell1->parameters["\\A_SIGNED"] = 0;
			cell1->parameters["\\B_SIGNED"] = 0;
			cell1->parameters["\\A_WIDTH"] = 1;
			cell1->parameters["\\B_WIDTH"] = 1;
			cell1->parameters["\\Y_WIDTH"] = 2;
			cell1->connections["\\A"] = net_map.at(inst->GetInput1());
			cell1->connections["\\B"] = net_map.at(inst->GetInput2());
			cell1->connections["\\Y"] = module->new_wire(2, NEW_ID);
			module->add(cell1);

			RTLIL::Cell *cell2 = new RTLIL::Cell;
			cell2->name = RTLIL::escape_id(inst->Name());
			cell2->type = "$add";
			cell2->parameters["\\A_SIGNED"] = 0;
			cell2->parameters["\\B_SIGNED"] = 0;
			cell2->parameters["\\A_WIDTH"] = 2;
			cell2->parameters["\\B_WIDTH"] = 1;
			cell2->parameters["\\Y_WIDTH"] = 2;
			cell2->connections["\\A"] = cell1->connections["\\Y"];
			cell2->connections["\\B"] = net_map.at(inst->GetCin());
			cell2->connections["\\Y"] = net_map.at(inst->GetOutput());
			cell2->connections["\\Y"].append(net_map.at(inst->GetCout()));
			module->add(cell2);
			continue;
		}

		if (inst->IsPrimitive())
			log_error("Unsupported Verific primitive: %s\n", inst->View()->Owner()->Name());

		nl_todo.insert(inst->View());

		RTLIL::Cell *cell = new RTLIL::Cell;
		cell->name = RTLIL::escape_id(inst->Name());
		cell->type = RTLIL::escape_id(inst->View()->Owner()->Name());
		module->add(cell);

		PortRef *pr ;
		FOREACH_PORTREF_OF_INST(inst, mi2, pr) {
			// log("      .%s(%s)\n", pr->GetPort()->Name(), pr->GetNet()->Name());
			const char *port_name = pr->GetPort()->Name();
			int port_offset = 0;
			if (pr->GetPort()->Bus()) {
				port_name = pr->GetPort()->Bus()->Name();
				port_offset = pr->GetPort()->Bus()->IndexOf(pr->GetPort()) -
						std::min(pr->GetPort()->Bus()->LeftIndex(), pr->GetPort()->Bus()->RightIndex());
			}
			RTLIL::SigSpec &conn = cell->connections[RTLIL::escape_id(port_name)];
			while (conn.width <= port_offset)
				conn.append(RTLIL::State::Sz);
			conn.replace(port_offset, net_map.at(pr->GetNet()));
		}
	}
}

#endif /* VERIFIC_DIR */

struct VerificPass : public Pass {
	VerificPass() : Pass("verific", "load Verilog and VHDL designs using Verific") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    verific {-vlog95|-vlog2k|-sv2005|-sv2009|-sv} <verilog-file>..\n");
		log("\n");
		log("Load the specified Verilog/SystemVerilog files into Verific.\n");
		log("\n");
		log("\n");
		log("    verific {-vhdl87|-vhdl93|-vhdl2k|-vhdl2008} <vhdl-file>..\n");
		log("\n");
		log("Load the specified VHDL files into Verific.\n");
		log("\n");
		log("\n");
		log("    verific -import <top-module>..\n");
		log("\n");
		log("Elaborate the design for the sepcified top modules, import to Yosys and\n");
		log("reset the internal state of Verific.\n");
		log("\n");
		log("\n");
		log("Visit http://verific.com/ for more information on Verific.\n");
		log("\n");
	}
#ifdef VERIFIC_DIR
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing VERIFIC (loading Verilog and VHDL designs using the Verific library).\n");

		Message::SetConsoleOutput(0);
		Message::RegisterCallBackMsg(msg_func);

		if (args.size() > 1 && args[1] == "-vlog95") {
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!veri_file::Analyze(args[argidx].c_str(), veri_file::VERILOG_95))
					log_cmd_error("Reading `%s' in VERILOG_95 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-vlog2k") {
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!veri_file::Analyze(args[argidx].c_str(), veri_file::VERILOG_2K))
					log_cmd_error("Reading `%s' in VERILOG_2K mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-sv2005") {
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!veri_file::Analyze(args[argidx].c_str(), veri_file::SYSTEM_VERILOG_2005))
					log_cmd_error("Reading `%s' in SYSTEM_VERILOG_2005 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-sv2009") {
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!veri_file::Analyze(args[argidx].c_str(), veri_file::SYSTEM_VERILOG_2009))
					log_cmd_error("Reading `%s' in SYSTEM_VERILOG_2009 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-sv") {
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!veri_file::Analyze(args[argidx].c_str(), veri_file::SYSTEM_VERILOG))
					log_cmd_error("Reading `%s' in SYSTEM_VERILOG mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-vhdl87") {
			vhdl_file::SetDefaultLibraryPath(VERIFIC_DIR "/vhdl_packages/vdbs");
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!vhdl_file::Analyze(args[argidx].c_str(), "work", vhdl_file::VHDL_87))
					log_cmd_error("Reading `%s' in VHDL_87 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-vhdl93") {
			vhdl_file::SetDefaultLibraryPath(VERIFIC_DIR "/vhdl_packages/vdbs");
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!vhdl_file::Analyze(args[argidx].c_str(), "work", vhdl_file::VHDL_93))
					log_cmd_error("Reading `%s' in VHDL_93 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-vhdl2k") {
			vhdl_file::SetDefaultLibraryPath(VERIFIC_DIR "/vhdl_packages/vdbs");
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!vhdl_file::Analyze(args[argidx].c_str(), "work", vhdl_file::VHDL_2K))
					log_cmd_error("Reading `%s' in VHDL_2K mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-vhdl2008") {
			vhdl_file::SetDefaultLibraryPath(VERIFIC_DIR "/vhdl_packages/vdbs");
			for (size_t argidx = 2; argidx < args.size(); argidx++)
				if (!vhdl_file::Analyze(args[argidx].c_str(), "work", vhdl_file::VHDL_2008))
					log_cmd_error("Reading `%s' in VHDL_2008 mode failed.\n", args[argidx].c_str());
			return;
		}

		if (args.size() > 1 && args[1] == "-import")
		{
			std::set<Netlist*> nl_todo, nl_done;

			if (args.size() == 2)
				log_cmd_error("No top module specified.\n");

			for (size_t argidx = 2; argidx < args.size(); argidx++) {
				if (veri_file::GetModule(args[argidx].c_str())) {
					if (!veri_file::Elaborate(args[argidx].c_str()))
						log_cmd_error("Elaboration of top module `%s' failed.\n", args[argidx].c_str());
					nl_todo.insert(Netlist::PresentDesign());
				} else {
					if (!vhdl_file::Elaborate(args[argidx].c_str()))
						log_cmd_error("Elaboration of top module `%s' failed.\n", args[argidx].c_str());
					nl_todo.insert(Netlist::PresentDesign());
				}
			}

			while (!nl_todo.empty()) {
				Netlist *nl = *nl_todo.begin();
				if (nl_done.count(nl) == 0)
					import_netlist(design, nl, nl_todo);
				nl_todo.erase(nl);
				nl_done.insert(nl);
			}

			Libset::Reset();
			return;
		}

		log_cmd_error("Missing or unsupported mode parameter.\n");
	}
#else /* VERIFIC_DIR */
	virtual void execute(std::vector<std::string>, RTLIL::Design *) {
		log_cmd_error("This version of Yosys is built without Verific support.\n");
	}
#endif
} VerificPass;
 
