//---------------------------------------------------------------------------//
// Copyright (c) 2021 Mikhail Komarov <nemo@nil.foundation>
// Copyright (c) 2021 Nikita Kaskov <nbering@nil.foundation>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//---------------------------------------------------------------------------//

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#ifndef BOOST_FILESYSTEM_NO_DEPRECATED
#define BOOST_FILESYSTEM_NO_DEPRECATED
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED
#define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/json/src.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include <nil/crypto3/algebra/curves/pallas.hpp>
#include <nil/crypto3/algebra/fields/arithmetic_params/pallas.hpp>
#include <nil/crypto3/algebra/curves/ed25519.hpp>
#include <nil/crypto3/algebra/fields/arithmetic_params/ed25519.hpp>

#include <nil/crypto3/zk/snark/arithmetization/plonk/params.hpp>
#include <nil/crypto3/zk/snark/arithmetization/plonk/constraint_system.hpp>

#include <ios>

#include <nil/marshalling/status_type.hpp>
#include <nil/marshalling/field_type.hpp>
#include <nil/marshalling/endianness.hpp>
#include <nil/crypto3/marshalling/zk/types/plonk/constraint_system.hpp>

#include <nil/blueprint/parser.hpp>
#include <nil/blueprint/asserts.hpp>
#include <nil/blueprint/transpiler/table_profiling.hpp>
#include <nil/blueprint/utils/satisfiability_check.hpp>

#include <llvm/Support/CommandLine.h>

using namespace nil;
using namespace nil::crypto3;

template<typename TIter>
void print_hex_byteblob(std::ostream &os, TIter iter_begin, TIter iter_end, bool endl) {
    os << "0x" << std::hex;
    for (TIter it = iter_begin; it != iter_end; it++) {
        os << std::setfill('0') << std::setw(2) << std::right << int(*it);
    }
    os << std::dec;
    if (endl) {
        os << std::endl;
    }
}

template<typename Endianness, typename ConstraintSystemType>
void print_circuit(const ConstraintSystemType &circuit, std::ostream &out = std::cout) {

    using TTypeBase = nil::marshalling::field_type<Endianness>;
    auto filled_val =
        nil::crypto3::marshalling::types::fill_plonk_constraint_system<ConstraintSystemType, Endianness>(circuit);

    std::vector<std::uint8_t> cv;
    cv.resize(filled_val.length(), 0x00);
    auto write_iter = cv.begin();
    nil::marshalling::status_type status = filled_val.write(write_iter, cv.size());
    print_hex_byteblob(out, cv.cbegin(), cv.cend(), false);
}

template<typename CurveType, typename ArithmetizationParams>
int curve_dependent_main(std::string bytecode_file_name,
                          std::string public_input_file_name,
                          std::string assignment_table_file_name,
                          std::string circuit_file_name,
                          bool check_validity) {
    using BlueprintFieldType = typename CurveType::base_field_type;
    using ConstraintSystemType = zk::snark::plonk_constraint_system<BlueprintFieldType, ArithmetizationParams>;

    std::vector<typename BlueprintFieldType::value_type> public_input;
    std::ifstream input_file(public_input_file_name.c_str());
    if (!input_file.is_open()) {
        std::cerr << "Could not open the file - '" << public_input_file_name << "'" << std::endl;
        return EXIT_FAILURE;
    }

    boost::json::stream_parser p;
    boost::json::error_code ec;
    while (!input_file.eof()) {
        char input_string[256];
        input_file.read(input_string, sizeof(input_string) - 1);
        input_string[input_file.gcount()] = '\0';
        p.write(input_string, ec);
        if (ec) {
            std::cerr << "JSON parsing of public input failed" << std::endl;
            return 1;
        }
    }
    p.finish(ec);
    if (ec) {
        std::cerr << "JSON parsing of public input failed" << std::endl;
        return 1;
    }
    boost::json::value input_json_value = p.release();
    if (!input_json_value.is_array()) {
        std::cerr << "Array of arguments is expected in JSON file" << std::endl;
        return 1;
    }

    nil::blueprint::parser<BlueprintFieldType, ArithmetizationParams> parser_instance;

    const char *llvm_arguments[2] = {"", "-opaque-pointers=0"};
    llvm::cl::ParseCommandLineOptions(2, llvm_arguments);

    std::unique_ptr<llvm::Module> module = parser_instance.parseIRFile(bytecode_file_name.c_str());
    if (module == nullptr) {
        return 1;
    }

    if (!parser_instance.evaluate(*module, input_json_value.as_array())) {
        return 1;
    }

    zk::snark::plonk_table_description<BlueprintFieldType, ArithmetizationParams> desc;
    desc.usable_rows_amount = parser_instance.assignmnt.rows_amount();
    desc.rows_amount = zk::snark::basic_padding(parser_instance.assignmnt);

    std::ofstream otable;
    otable.open(assignment_table_file_name);
    if (!otable) {
        std::cout << "Something wrong with output " << assignment_table_file_name << std::endl;
        return 1;
    }
    nil::blueprint::profiling_assignment_table(parser_instance.assignmnt, desc.usable_rows_amount, otable);
    otable.close();

    std::ofstream ocircuit;
    ocircuit.open(circuit_file_name);
    if (!ocircuit) {
        std::cout << "Something wrong with output " << circuit_file_name << std::endl;
        return 1;
    }
    print_circuit<nil::marshalling::option::big_endian, ConstraintSystemType>(parser_instance.bp, ocircuit);
    ocircuit.close();

    if (check_validity){
        bool is_satisfied = nil::blueprint::is_satisfied(parser_instance.bp, parser_instance.assignmnt);
        ASSERT_MSG(is_satisfied, "The circuit is not satisfied" );
    }

    return 0;
}

int main(int argc, char *argv[]) {

    boost::program_options::options_description options_desc("zkLLVM assigner");

    // clang-format off
    options_desc.add_options()("help,h", "Display help message")
            ("version,v", "Display version")
            ("bytecode,b", boost::program_options::value<std::string>(), "Bytecode input file")
            ("public-input,i", boost::program_options::value<std::string>(), "Public input file")
            ("assignment-table,t", boost::program_options::value<std::string>(), "Assignment table output file")
            ("circuit,c", boost::program_options::value<std::string>(), "Circuit output file")
            ("elliptic-curve-type,e", boost::program_options::value<std::string>(), "Native elliptic curve type (pallas, vesta, ed25519, bls12-381). If not set, pallas is used by default")
            ("arithmetization-setting,s", boost::program_options::value<std::string>(), "Arithmetization setting: standard, tiny or wide. If not set, standard is used by default")
            ("check", "Check satisfiability of the generated circuit");
    // clang-format on

    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(options_desc).run(),
                                  vm);
    boost::program_options::notify(vm);

    if (vm.count("help")) {
        std::cout << options_desc << std::endl;
        return 0;
    }

    std::string bytecode_file_name;
    std::string public_input_file_name;
    std::string assignment_table_file_name;
    std::string circuit_file_name;
    std::string elliptic_curve;
    std::string arithmetization_setting;

    if (vm.count("bytecode")) {
        bytecode_file_name = vm["bytecode"].as<std::string>();
    } else {
        std::cerr << "Invalid command line argument - bytecode file name is not specified" << std::endl;
        std::cout << options_desc << std::endl;
        return 1;
    }

    if (vm.count("public-input")) {
        public_input_file_name = vm["public-input"].as<std::string>();
    } else {
        std::cerr << "Invalid command line argument - public input file name is not specified" << std::endl;
        std::cout << options_desc << std::endl;
        return 1;
    }

    if (vm.count("assignment-table")) {
        assignment_table_file_name = vm["assignment-table"].as<std::string>();
    } else {
        std::cerr << "Invalid command line argument - assignment table file name is not specified" << std::endl;
        std::cout << options_desc << std::endl;
        return 1;
    }

    if (vm.count("circuit")) {
        circuit_file_name = vm["circuit"].as<std::string>();
    } else {
        std::cerr << "Invalid command line argument - circuit file name is not specified" << std::endl;
        std::cout << options_desc << std::endl;
        return 1;
    }

    if (vm.count("elliptic-curve-type")) {
        elliptic_curve = vm["elliptic-curve-type"].as<std::string>();
    } else {
        std::cout << "Command line argument -e (Native elliptic curve type) not recognized: " << elliptic_curve << ". Pallas field is used by default." << std::endl;
        elliptic_curve = "pallas";
    }

    std::map<std::string, int> curve_options{
        {"pallas", 0},
        {"vesta", 1},
        {"ed25519", 2},
        {"bls12-381", 3},
    };
    
    if (vm.count("arithmetization-setting")){
        arithmetization_setting = vm["arithmetization-setting"].as<std::string>();
    } else {
        std::cout << "Command line argument -s (Arithmetization setting) not recognized: " << arithmetization_setting << ". Standard arithmetization setting is used by default." << std::endl;
        arithmetization_setting = "standard";
    }

    std::map<std::string, int> arithmetization_options{
        {"tiny", 0},
        {"standard", 1},
        {"wide", 2},
    };
    
    using wt9pi1cn1 = zk::snark::plonk_arithmetization_params<9,1,1,50>;
    using wt15pi5cn5 = zk::snark::plonk_arithmetization_params<15,5,5,50>;
    using wt150pi5cn1 = zk::snark::plonk_arithmetization_params<150,5,1,50>;

    using arithmetization_type = wt15pi5cn5;

    switch (curve_options[elliptic_curve]) {
        case 0: {
            switch(arithmetization_options[arithmetization_setting]){
                case 0: {
                    using arithmetization_type = wt9pi1cn1;
                    return curve_dependent_main<typename algebra::curves::pallas, arithmetization_type>(bytecode_file_name, public_input_file_name, assignment_table_file_name, circuit_file_name, vm.count("check"));
                    break;
                }
                case 1: {
                    using arithmetization_type = wt15pi5cn5;
                    return curve_dependent_main<typename algebra::curves::pallas, arithmetization_type>(bytecode_file_name, public_input_file_name, assignment_table_file_name, circuit_file_name, vm.count("check"));
                    break;
                }
                case 2: {
                    using arithmetization_type = wt150pi5cn1;
                    return curve_dependent_main<typename algebra::curves::pallas, arithmetization_type>(bytecode_file_name, public_input_file_name, assignment_table_file_name, circuit_file_name, vm.count("check"));
                    break;
                }
            };            
            break;
        }
        case 1: {
            std::cerr << "command line argument -e vesta is not supported yet" << std::endl;
            assert(1==0 && "vesta curve based circuits are not supported yet");
            break;
        }
        case 2: {
            std::cerr << "command line argument -e ed25519 is not supported yet" << std::endl;
            assert(1==0 && "ed25519 curve based circuits are not supported yet");
            break;
        }
        case 3: {
            std::cerr << "command line argument -e bls12-381 is not supported yet" << std::endl;
            assert(1==0 && "bls12-381 curve based circuits are not supported yet");
            break;
        }
    };

    return 0;
}
