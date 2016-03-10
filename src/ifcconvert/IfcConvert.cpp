﻿/********************************************************************************
 *                                                                              *
 * This file is part of IfcOpenShell.                                           *
 *                                                                              *
 * IfcOpenShell is free software: you can redistribute it and/or modify         *
 * it under the terms of the Lesser GNU General Public License as published by  *
 * the Free Software Foundation, either version 3.0 of the License, or          *
 * (at your option) any later version.                                          *
 *                                                                              *
 * IfcOpenShell is distributed in the hope that it will be useful,              *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
 * Lesser GNU General Public License for more details.                          *
 *                                                                              *
 * You should have received a copy of the Lesser GNU General Public License     *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.         *
 *                                                                              *
 ********************************************************************************/

/********************************************************************************
 *                                                                              *
 * This started as a brief example of how IfcOpenShell can be interfaced from   * 
 * within a C++ context, it has since then evolved into a fullfledged command   *
 * line application that is able to convert geometry in an IFC files into       *
 * several tesselated and topological output formats.                           *
 *                                                                              *
 ********************************************************************************/

#include "../ifcconvert/ColladaSerializer.h"
#include "../ifcconvert/IgesSerializer.h"
#include "../ifcconvert/StepSerializer.h"
#include "../ifcconvert/WavefrontObjSerializer.h"
#include "../ifcconvert/XmlSerializer.h"
#include "../ifcconvert/SvgSerializer.h"

#include "../ifcgeom/IfcGeomIterator.h"

#include <IGESControl_Controller.hxx>
#include <Standard_Version.hxx>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/optional/optional_io.hpp>

#include <fstream>
#include <sstream>
#include <set>
#include <time.h>

#define INF std::numeric_limits<real_t>::infinity()
real_t bounds_min[3] = { INF, INF, INF };
real_t bounds_max[3] = { -INF, -INF, -INF };

#if USE_VLD
#include <vld.h>
#endif

const std::string DEFAULT_EXTENSION = "obj";

void print_version()
{
    /// @todo Why cerr used for info prints? Change to cout.
    std::cerr << "IfcOpenShell " << IfcSchema::Identifier << " IfcConvert " << IFCOPENSHELL_VERSION << std::endl;
}

void print_usage()
{
    std::cerr << "Usage: IfcConvert [options] <input.ifc> [<output>]" << "\n"
        << "\n"
        << "Converts the geometry in an IFC file into one of the following formats:" << "\n"
        << "  .obj   WaveFront OBJ  (a .mtl file is also created)" << "\n"
#ifdef WITH_OPENCOLLADA
        << "  .dae   Collada        Digital Assets Exchange" << "\n"
#endif
        << "  .stp   STEP           Standard for the Exchange of Product Data" << "\n"
        << "  .igs   IGES           Initial Graphics Exchange Specification" << "\n"
        << "  .xml   XML            Property definitions and decomposition tree" << "\n"
        << "  .svg   SVG            Scalable Vector Graphics (2D floor plan)" << "\n"
        << "\n"
        << "If no output filename given, <input>." + DEFAULT_EXTENSION + " will be used as the output file.\n"
        << "\n"
        << "Run 'IfcConvert --help' for more information." << std::endl;
}

void print_options(
    const boost::optional<boost::program_options::options_description>& generic_options,
    const boost::optional<boost::program_options::options_description>& geom_options,
    const boost::optional<boost::program_options::options_description>& serialization_options)
{
    if (generic_options)
        std::cerr << "\nCommand line options\n" << generic_options;
    if (geom_options)
        std::cerr << "\nGeometry options\n" << geom_options;
    if (serialization_options)
        std::cerr << "\nSerialization options\n" << serialization_options;
    std::cerr << std::endl;
}

std::string change_extension(const std::string& fn, const std::string& ext) {
	std::string::size_type dot = fn.find_last_of('.');
	if (dot != std::string::npos) {
		return fn.substr(0,dot+1) + ext;
	} else {
		return fn + "." + ext;
	}
}

static std::stringstream log_stream;
void write_log();

int main(int argc, char** argv) {
	boost::program_options::options_description generic_options;
	generic_options.add_options()
		("help,h", "display usage information")
		("version", "display version information")
		("verbose,v", "more verbose output");

	boost::program_options::options_description fileio_options;
	fileio_options.add_options()
		("input-file", boost::program_options::value<std::string>(), "input IFC file")
		("output-file", boost::program_options::value<std::string>(), "output geometry file");

    std::vector<std::string> entity_vector/*, names*/;
    //double deflection_tolerance;
	boost::program_options::options_description geom_options;
	geom_options.add_options()
		("plan",
			"Specifies whether to include curves in the output result. Typically "
			"these are representations of type Plan or Axis. Excluded by default.")
		("model",
			"Specifies whether to include surfaces and solids in the output result. "
			"Typically these are representations of type Body or Facetation. "
			"Included by default.")
		("weld-vertices",
			"Specifies whether vertices are welded, meaning that the coordinates "
			"vector will only contain unique xyz-triplets. This results in a "
			"manifold mesh which is useful for modelling applications, but might "
			"result in unwanted shading artefacts in rendering applications.")
		("use-world-coords", 
			"Specifies whether to apply the local placements of building elements "
			"directly to the coordinates of the representation mesh rather than "
			"to represent the local placement in the 4x3 matrix, which will in that "
			"case be the identity matrix.")
		("convert-back-units",
			"Specifies whether to convert back geometrical output back to the "
			"unit of measure in which it is defined in the IFC file. Default is "
			"to use meters.")
		("sew-shells", 
			"Specifies whether to sew the faces of IfcConnectedFaceSets together. "
			"This is a potentially time consuming operation, but guarantees a "
			"consistent orientation of surface normals, even if the faces are not "
			"properly oriented in the IFC file.")
#if OCC_VERSION_HEX < 0x60900
		// In Open CASCADE version prior to 6.9.0 boolean operations with multiple
		// arguments where not introduced yet and a work-around was implemented to
		// subtract multiple openings as a single compound. This hack is obsolete
		// for newer versions of Open CASCADE.
		("merge-boolean-operands", 
			"Specifies whether to merge all IfcOpeningElement operands into a single "
			"operand before applying the subtraction operation. This may "
			"introduce a performance improvement at the risk of failing, in "
			"which case the subtraction is applied one-by-one.")
#endif
		("disable-opening-subtractions", 
			"Specifies whether to disable the boolean subtraction of "
			"IfcOpeningElement Representations from their RelatingElements.")
		("include", 
            "Specifies that the entities listed after --entities or --names are to be included")
		("exclude", 
            "Specifies that the entities listed after --entities or --names are to be excluded")
		("entities", boost::program_options::value< std::vector<std::string> >(&entity_vector)->multitoken(),
			"A list of entities that should be included in or excluded from the "
			"geometrical output, depending on whether --exclude or --include is specified. "
			" Defaults to IfcOpeningElement and IfcSpace to be excluded. "
            "The names are handled case-insensitively. Cannot be placed right before input file argument.")
        /*("names", boost::program_options::value< std::vector<std::string> >(&names)->multitoken(),
            "A list of names or wildcard patterns that should be included in or excluded from the "
            "geometrical output, depending on whether --exclude or --include is specified. "
            "The names are handled case-sensitively. Cannot be placed right before input file argument.")
        ("no-normals",
            "Disables computation of normals. Saves time and file size and is useful "
            "in instances where you're going to recompute normals for the exported "
            "model in other modelling application in any case.")
        ("deflection-tolerance", boost::program_options::value<double>(&deflection_tolerance),
            "Sets the deflection tolerance of the mesher, 1e-3 by default if not specified.")*/;

    std::string bounds;
    boost::program_options::options_description serializer_options;
    serializer_options.add_options()
        ("bounds", boost::program_options::value<std::string>(&bounds),
            "Specifies the bounding rectangle, for example 512x512, to which the "
            "output will be scaled. Only used when converting to SVG.")
        ("use-names",
            "Use entity names instead of unique IDs for naming objects and materials "
            "upon serialization. Applicable for .obj and .dae output.")
        ("use-guids",
            "Use entity GUIDs instead of unique IDs for naming objects upon serialization. "
            "Overrides possible usage of --use-names for objects but not for materials."
            "Applicable for .obj and .dae output.")
        /*("center-model",
            "Centers the models upon serialization by applying the center point of "
            "the scene bounds as an offset. Applicable only for .dae output currently.")
        ("generate-uvs",
        "Generates UVs (texture coordinates) by using simple box projection. Requires normals. Not guaranteed to work "
            "properly if used with --weld-vertices. Applicable only for .dae output currently.")*/;

	boost::program_options::options_description cmdline_options;
	cmdline_options.add(generic_options).add(fileio_options).add(geom_options).add(serializer_options);

	boost::program_options::positional_options_description positional_options;
	positional_options.add("input-file", 1);
	positional_options.add("output-file", 1);

	boost::program_options::variables_map vmap;
	try {
		boost::program_options::store(boost::program_options::command_line_parser(argc, argv).
				  options(cmdline_options).positional(positional_options).run(), vmap);
	} catch (const boost::program_options::unknown_option& e) {
		std::cerr << "[Error] Unknown option '" << e.get_option_name() << "'" << std::endl << std::endl;
        print_usage();
        return 1;
	} catch (...) {
		// Catch other errors such as invalid command line syntax
        print_usage();
        return 1;
	}
	boost::program_options::notify(vmap);

    print_version();

    if (vmap.count("version")) {
        return 0;
    } else if (vmap.count("help")) {
        print_usage();
        print_options(generic_options, geom_options, serializer_options);
        return 0;
    } else if (!vmap.count("input-file")) {
        std::cerr << "[Error] Input file not specified" << std::endl;
        print_usage();
        return 1;
	} else if (vmap.count("include") && vmap.count("exclude")) {
        std::cerr << "[Error] --include and --exclude can not be specified together" << std::endl;
        print_options(boost::none, geom_options, boost::none);
		return 1;
	}

	const bool verbose = vmap.count("verbose") != 0;
	const bool weld_vertices = vmap.count("weld-vertices") != 0;
	const bool use_world_coords = vmap.count("use-world-coords") != 0;
	const bool convert_back_units = vmap.count("convert-back-units") != 0;
	const bool sew_shells = vmap.count("sew-shells") != 0;
#if OCC_VERSION_HEX < 0x60900
	const bool merge_boolean_operands = vmap.count("merge-boolean-operands") != 0;
#endif
	const bool disable_opening_subtractions = vmap.count("disable-opening-subtractions") != 0;
	bool include_entities = vmap.count("include") != 0;
	const bool include_plan = vmap.count("plan") != 0;
	const bool include_model = vmap.count("model") != 0 || (!include_plan);
    const bool use_names = vmap.count("use-names") != 0;
    const bool use_guids = vmap.count("use-guids") != 0 ;
    //const bool no_normals = vmap.count("no-normals") != 0 ;
    //const bool center_model = vmap.count("center-model") != 0 ;
    //const bool generate_uvs = vmap.count("generate-uvs") != 0 ;
    //const bool deflection_tolerance_specified = vmap.count("deflection-tolerance") != 0 ;
	boost::optional<int> bounding_width, bounding_height;
	if (vmap.count("bounds") == 1) {
		int w, h;
		if (sscanf(bounds.c_str(), "%ux%u", &w, &h) == 2) {
			bounding_width = w;
			bounding_height = h;
		} else {
			std::cerr << "[Error] Invalid use of --bounds" << std::endl;
			print_options(boost::none, boost::none, serializer_options);
			return 1;
		}
	}

	// Gets the set ifc types to be ignored from the command line. 
	std::set<std::string> entities(entity_vector.begin(), entity_vector.end());

	const std::string input_filename = vmap["input-file"].as<std::string>();
	// If no output filename is specified a Wavefront OBJ file will be output
	// to maintain backwards compatibility with the obsolete IfcObj executable.
	const std::string output_filename = vmap.count("output-file") == 1 
		? vmap["output-file"].as<std::string>()
		: change_extension(input_filename, DEFAULT_EXTENSION);
	
	if (output_filename.size() < 5) {
        std::cerr << "[Error] Invalid or unsupported output file given " << output_filename << std::endl;
        print_usage();
		return 1;
	}

	std::string output_extension = output_filename.substr(output_filename.size()-4);
	boost::to_lower(output_extension);

	// If no entities are specified these are the defaults to skip from output
	if (entities.empty()) {
        entities.insert("IfcSpace");
        /// @todo Document in --help that SVG uses "--include --entities IfcSpace" by default.
		if (output_extension == ".svg") {
			include_entities = true;
		} else {
            entities.insert("IfcOpeningElement");
		}
	}

	Logger::SetOutput(&std::cout, &log_stream);
	Logger::Verbosity(verbose ? Logger::LOG_NOTICE : Logger::LOG_ERROR);

	if (output_extension == ".xml") {
		int exit_code = 1;
		try {
			XmlSerializer s(output_filename);
			IfcParse::IfcFile f;
			if (!f.Init(input_filename)) {
				Logger::Message(Logger::LOG_ERROR, "Unable to parse .ifc file");
			} else {
				s.setFile(&f);
				s.finalize();
				exit_code = 0;
			}
		} catch (...) {}
		write_log();
		return exit_code;
	}

	IfcGeom::IteratorSettings settings;
	/// @todo Make APPLY_DEFAULT_MATERIALS configurable? Quickly tested setting this to false and using obj exporter caused the program to crash and burn.
	settings.set(IfcGeom::IteratorSettings::APPLY_DEFAULT_MATERIALS,      true);
	settings.set(IfcGeom::IteratorSettings::USE_WORLD_COORDS,             use_world_coords);
	settings.set(IfcGeom::IteratorSettings::WELD_VERTICES,                weld_vertices);
	settings.set(IfcGeom::IteratorSettings::SEW_SHELLS,                   sew_shells);
	settings.set(IfcGeom::IteratorSettings::CONVERT_BACK_UNITS,           convert_back_units);
#if OCC_VERSION_HEX < 0x60900
	settings.set(IfcGeom::IteratorSettings::FASTER_BOOLEANS,              merge_boolean_operands);
#endif
	settings.set(IfcGeom::IteratorSettings::DISABLE_OPENING_SUBTRACTIONS, disable_opening_subtractions);
	settings.set(IfcGeom::IteratorSettings::INCLUDE_CURVES,               include_plan);
	settings.set(IfcGeom::IteratorSettings::EXCLUDE_SOLIDS_AND_SURFACES,  !include_model);
    settings.set(IfcGeom::IteratorSettings::USE_NAMES, use_names);
    settings.set(IfcGeom::IteratorSettings::USE_GUIDS, use_guids);
    //settings.set(IfcGeom::IteratorSettings::NO_NORMALS, no_normals);
    //settings.set(IfcGeom::IteratorSettings::CENTER_MODEL, center_model);
    //settings.set(IfcGeom::IteratorSettings::GENERATE_UVS, generate_uvs);
    //if (deflection_tolerance_specified)
    //      settings.set_deflection_tolerance(deflection_tolerance);

	GeometrySerializer* serializer;
	if (output_extension == ".obj") {
		const std::string mtl_filename = output_filename.substr(0,output_filename.size()-3) + "mtl";
		if (!use_world_coords) {
			Logger::Message(Logger::LOG_NOTICE, "Using world coords when writing WaveFront OBJ files");
			settings.set(IfcGeom::IteratorSettings::USE_WORLD_COORDS, true);
		}
		serializer = new WaveFrontOBJSerializer(output_filename, mtl_filename, settings);
#ifdef WITH_OPENCOLLADA
	} else if (output_extension == ".dae") {
		serializer = new ColladaSerializer(output_filename, settings);
#endif
	} else if (output_extension == ".stp") {
		serializer = new StepSerializer(output_filename, settings);
	} else if (output_extension == ".igs") {
		// Not sure why this is needed, but it is.
		// See: http://tracker.dev.opencascade.org/view.php?id=23679
		IGESControl_Controller::Init();
		serializer = new IgesSerializer(output_filename, settings);
	} else if (output_extension == ".svg") {
		settings.set(IfcGeom::IteratorSettings::DISABLE_TRIANGULATION, true);
		serializer = new SvgSerializer(output_filename, settings);
		if (bounding_width && bounding_height) {
            static_cast<SvgSerializer*>(serializer)->setBoundingRectangle(*bounding_width, *bounding_height);
		}
	} else {
		Logger::Message(Logger::LOG_ERROR, "Unknown output filename extension");
		write_log();
		print_usage();
		return 1;
	}

	if (!serializer->isTesselated()) {
		if (weld_vertices) {
			Logger::Message(Logger::LOG_NOTICE, "Weld vertices setting ignored when writing STEP or IGES files");
		}
        settings.set(IfcGeom::IteratorSettings::DISABLE_TRIANGULATION, true);
	}

    IfcGeom::Iterator<real_t> context_iterator(settings, input_filename);

	try {
		if (include_entities) {
			context_iterator.includeEntities(entities);
            //context_iterator.include_entity_names(names);
		} else {
			context_iterator.excludeEntities(entities);
            //context_iterator.exclude_entity_names(names);
		}
	} catch (const IfcParse::IfcException& e) {
		std::cout << "[Error] " << e.what() << std::endl;
		return 1;
	}

	if (!serializer->ready()) {
		Logger::Message(Logger::LOG_ERROR, "Unable to open output file for writing");
		write_log();
		return 1;
	}

	time_t start,end;
	time(&start);
	
	if (!context_iterator.initialize()) {
		Logger::Message(Logger::LOG_ERROR, "Unable to parse .ifc file or no geometrical entities found");
		write_log();
		return 1;
	}

    serializer->setFile(context_iterator.getFile());

	if (convert_back_units) {
		serializer->setUnitNameAndMagnitude(context_iterator.getUnitName(), static_cast<float>(context_iterator.getUnitMagnitude()));
	} else {
		serializer->setUnitNameAndMagnitude("METER", 1.0f);
	}

	serializer->writeHeader();

	int old_progress = -1;
	Logger::Status("Creating geometry...");

    std::vector<IfcGeom::Element<real_t>* > geometries;
	// The functions IfcGeom::Iterator::get() and IfcGeom::Iterator::next() 
	// wrap an iterator of all geometrical products in the Ifc file. 
	// IfcGeom::Iterator::get() returns an IfcGeom::TriangulationElement or 
	// -BRepElement pointer, based on current settings. (see IfcGeomIterator.h 
	// for definition) IfcGeom::Iterator::next() is used to poll whether more 
	// geometrical entities are available. None of these functions throw 
	// exceptions, neither for parsing errors or geometrical errors. Upon 
	// calling next() the entity to be returned has already been processed, a 
	// true return value guarantees that a successfully processed product is 
	// available. 
	do {
        IfcGeom::Element<real_t> *geom_object = context_iterator.get(true); // true == take ownership, we will clean up ourselves
        geometries.push_back(geom_object);
        const int progress = context_iterator.progress() / 2;
        if (old_progress != progress) Logger::ProgressBar(progress);
        old_progress = progress;

        //if (center_model) {
        //    const std::vector<real_t>& pos = geom_object->transformation().matrix().data();
        //    bounds_min[0] = std::min(bounds_min[0], pos[9]);
        //    bounds_min[1] = std::min(bounds_min[1], pos[10]);
        //    bounds_min[2] = std::min(bounds_min[2], pos[11]);
        //    bounds_max[0] = std::max(bounds_max[0], pos[9]);
        //    bounds_max[1] = std::max(bounds_max[1], pos[10]);
        //    bounds_max[2] = std::max(bounds_max[2], pos[11]);
        //}
    } while (context_iterator.next());

    Logger::Status("\rDone creating geometry (" + boost::lexical_cast<std::string>(geometries.size()) +
        " objects)                                ");

    //if (center_model) {
    //    settings.offset[0] = -(bounds_min[0] + bounds_max[0]) * real_t(0.5);
    //    settings.offset[1] = -(bounds_min[1] + bounds_max[1]) * real_t(0.5);
    //    settings.offset[2] = -(bounds_min[2] + bounds_max[2]) * real_t(0.5);
    //    //printf("Bounds min. (%g, %g, %g)\n", bounds_min[0], bounds_min[1], bounds_min[2]);
    //    //printf("Bounds max. (%g, %g, %g)\n", bounds_max[0], bounds_max[1], bounds_max[2]);
    //    printf("Using model offset (%g, %g, %g)\n", settings.offset[0], settings.offset[1], settings.offset[2]); //TODO Logger::Message(Logger::LOG_NOTICE, ...);
    //}

    Logger::Status("Serializing geometry...");

    if (serializer->isTesselated()) { // isTesselated() doesn't change at run-time
        foreach(const IfcGeom::Element<real_t>* geom, geometries) {
            serializer->write(static_cast<const IfcGeom::TriangulationElement<real_t>*>(geom));
            delete geom;
        }
    } else {
        foreach(const IfcGeom::Element<real_t>* geom, geometries) {
            serializer->write(static_cast<const IfcGeom::BRepElement<real_t>*>(geom));
            delete geom;
        }
    }

	serializer->finalize();

    Logger::Status("\rDone serializing geometry                                ");

	delete serializer;

	write_log();

	time(&end);
    int seconds = (int)difftime(end, start);
    if (seconds < 60)
        printf("\nConversion took %d seconds\n", seconds); // TODO Logger::Message(Logger::LOG_NOTICE, ...);
    else
        printf("\nConversion took %d minute(s) %d seconds\n", seconds/60, seconds%60); // TODO Logger::Message(Logger::LOG_NOTICE, ...);

	return 0;
}

void write_log() {
	std::string log = log_stream.str();
	if (!log.empty()) {
		std::cerr << "\n" << "Log:\n" << log << std::endl;
	}
}
