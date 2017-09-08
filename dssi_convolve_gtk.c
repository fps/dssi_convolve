#include <string.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
#include <stdio.h>
#include <lo/lo.h>
#include "math.h"
#include <lo/lo_lowlevel.h>

#include "defaults.h"


GladeXML        *xml;

#define          MAX_STRING_LENGTH 1024

char            *host_osc_url;

char            *host_base_path;

lo_address       host_address;

lo_server        server;

int              server_port;

char             tmp_string[MAX_STRING_LENGTH];
char             tmp_string2[MAX_STRING_LENGTH];

FILE            *file;

/* avoid feedback loops */	
int              we_did_it = 0;

void on_mainwindow_destroy_event(GtkWidget *widget, gpointer user_data) 
{
	sprintf(tmp_string, "%s/exiting", host_base_path);

	lo_send(host_address, tmp_string, NULL);

	gtk_main_quit();
}


void on_mainwindow_delete_event(GtkWidget *widget, gpointer user_data) 
{
	sprintf(tmp_string, "%s/exiting", host_base_path);

	lo_send(host_address, tmp_string, NULL);

	gtk_main_quit();
}

void on_gain_spinbutton_changed(GtkWidget *widget, gpointer user_data) 
{
	GtkSpinButton *spin;
	
	spin = (GtkSpinButton*)widget;
	
	if (!we_did_it) {
		sprintf(tmp_string, "%s/control", host_base_path);
		lo_send(host_address, tmp_string, "if", 0, gtk_spin_button_get_value(spin));
	}
	we_did_it = 0;
}

void on_dry_gain_spinbutton_changed(GtkWidget *widget, gpointer user_data) 
{
	GtkSpinButton *spin;
	
	spin = (GtkSpinButton*)widget;

	if (!we_did_it) {
		sprintf(tmp_string, "%s/control", host_base_path);
		lo_send(host_address, tmp_string, "if", 1, gtk_spin_button_get_value(spin));
	}
	we_did_it = 0;
}


void on_responsefile_dialog_ok(GtkWidget *widget, GtkFileSelection *selection) 
{
	gtk_entry_set_text((GtkEntry*)glade_xml_get_widget(xml, "responsefile_display"),
	                   gtk_file_selection_get_filename(selection));

	sprintf(tmp_string, "%s/configure", host_base_path);
	lo_send(host_address, tmp_string, "ss", "responsefile", gtk_file_selection_get_filename(selection));
}

void on_responsefile_button_clicked(GtkWidget *widget, gpointer user_data) 
{
	GtkWidget *dialog;
	dialog = gtk_file_selection_new ("Select response file..");

	g_signal_connect (G_OBJECT (GTK_FILE_SELECTION (dialog)->ok_button),
	                  "clicked", G_CALLBACK (on_responsefile_dialog_ok), (gpointer) dialog);

	g_signal_connect_swapped (G_OBJECT (GTK_FILE_SELECTION (dialog)->cancel_button),
	                          "clicked", G_CALLBACK (gtk_widget_destroy),
	                          G_OBJECT (dialog));

	g_signal_connect_swapped (G_OBJECT (GTK_FILE_SELECTION (dialog)->ok_button),
	                          "clicked", G_CALLBACK (gtk_widget_destroy),
	                          G_OBJECT (dialog));

	gtk_widget_show(dialog);
}

void on_priority_spinbutton_changed(GtkWidget *widget, gpointer user_data) 
{
	GtkSpinButton *spin;
	
	spin = (GtkSpinButton*)widget;

	if (!we_did_it) {	
		sprintf(tmp_string, "%s/configure", host_base_path);
		sprintf(tmp_string2, "%i", (int)gtk_spin_button_get_value(spin));

		lo_send(host_address, tmp_string, "ss", "rtprio", tmp_string2);
	}
	we_did_it = 0;
}

void on_partitionsize_combo_changed(GtkWidget *widget, gpointer user_data) 
{
	GtkComboBox *box;
	
	box = (GtkComboBox*)widget;

	if (!we_did_it) {
		sprintf(tmp_string, "%s/configure", host_base_path);
		sprintf(tmp_string2, "%s", gtk_combo_box_get_active_text(box));

		lo_send(host_address, tmp_string, "ss", "partitionsize", tmp_string2);
	}
	we_did_it = 0;
}

void on_showaboutbutton_clicked(GtkWidget *widget, gpointer user_data) 
{
//	gtk_show_about_dialog((GtkAboutDialog*)glade_xml_get_widget(xml, "priority_spinbutton"));
	gtk_widget_show((GtkWidget*)glade_xml_get_widget(xml, "aboutdialog1"));
}



void on_showhelpbutton_clicked(GtkWidget *widget, gpointer user_data) 
{
	gtk_widget_show((GtkWidget*)glade_xml_get_widget(xml, "helpwindow1"));

}

void error(int num, const char *m, const char *path) {
	fprintf(file, "error %s %s\n", m, path);
	fflush(file);
}

int generic_handler(const char *path, const char *types, lo_arg **argv,
		    int argc, void *data, void *user_data)
{
#if 0
    int i;

    printf("path: <%s>\n", path);
    for (i=0; i<argc; i++) {
		printf("arg %d '%c' ", i, types[i]);
		lo_arg_pp(types[i], argv[i]);
		printf("\n");
    }
    printf("\n");
    fflush(stdout);

#endif
    return 1;
}
int handle_configure(const char *path, const char *types,
                     lo_arg **argv, int argc, lo_message msg,
                     void *user_data)
{
	fprintf(file, "configure\n");
	fflush(file);
	if (strcmp(&argv[0]->s, "partitionsize") == 0) {
		fprintf(file, "partitionsize %s\n", &argv[1]->s);
		fflush(file);
		we_did_it = 1;
		gtk_combo_box_set_active((GtkComboBox*)glade_xml_get_widget(xml, "partitionsize_combo"), log2(atoi(&argv[1]->s)/64));

	}
	if (strcmp(&argv[0]->s, "rtprio") == 0) {
		we_did_it = 1;
		gtk_spin_button_set_value((GtkSpinButton*)glade_xml_get_widget(xml, "priority_spinbutton"), atoi(&argv[1]->s));
		fprintf(file, "rtprio %s\n", &argv[1]->s);
		fflush(file);
	}
	if (strcmp(&argv[0]->s, "responsefile") == 0) {
		fprintf(file, "responsefile %s\n", &argv[1]->s);
		fflush(file);
		we_did_it = 1;
		gtk_entry_set_text((GtkEntry*)glade_xml_get_widget(xml, "responsefile_display"),
			&argv[1]->s);
	}
	return 1;
}

int handle_show(const char *path, const char *types,
                     lo_arg **argv, int argc, lo_message msg,
                     void *user_data)
{
	gtk_widget_show((GtkWidget*)glade_xml_get_widget(xml, "mainwindow"));
	return 1;
}

int handle_hide(const char *path, const char *types,
                     lo_arg **argv, int argc, lo_message msg,
                     void *user_data)
{
	gtk_widget_hide((GtkWidget*)glade_xml_get_widget(xml, "mainwindow"));
	return 1;
}

int handle_quit(const char *path, const char *types,
                     lo_arg **argv, int argc, lo_message msg,
                     void *user_data)
{
	exit (0);

	return 1;
}

int handle_control(const char *path, const char *types,
                   lo_arg **argv, int argc, lo_message msg,
                   void *user_data)
{
	fprintf(file, "control: %i %f\n", argv[0]->i, argv[1]->f);
	fflush(file);
	switch(argv[0]->i) {
		case 0:
			we_did_it = 1;
			gtk_spin_button_set_value((GtkSpinButton*)glade_xml_get_widget(xml, "gain_spinbutton"), argv[1]->f);
			break;
		case 1:
			we_did_it = 1;
			gtk_spin_button_set_value((GtkSpinButton*)glade_xml_get_widget(xml, "dry_gain_spinbutton"), argv[1]->f);
			break;
		default:
			break;
	}
	return 0;
}


gboolean process_osc_messages(gpointer data) {
//	fprintf(file, "process\n");
	fflush(file);

	lo_server_recv_noblock(server, 0);

	return TRUE;
}

int main(int argc, char **argv)
{
#if 0
	int index;
#endif

	gtk_init(&argc, &argv);
	glade_init();

	file = fopen("/dev/stdout", "w+");

#if 0
	for (index = 0; (index < argc); ++index) {
		fprintf(file, "[%i]: %s\n", index, argv[index]);
		fflush(file);
	}
#endif

	host_address = lo_address_new(lo_url_get_hostname(argv[1]), lo_url_get_port(argv[1]));

	host_osc_url = argv[1];

	host_base_path = lo_url_get_path(argv[1]);

	server = lo_server_new(NULL, error);	

	server_port = lo_server_get_port(server);

	lo_server_add_method(server, NULL, NULL, generic_handler, NULL);
	lo_server_add_method(server, "//configure", "ss", handle_configure, NULL);
	lo_server_add_method(server, "//control", "if", handle_control, NULL);
	lo_server_add_method(server, "//show", NULL, handle_show, NULL);
	lo_server_add_method(server, "//hide", NULL, handle_hide, NULL);
	lo_server_add_method(server, "//quit", NULL, handle_quit, NULL);
	
	xml = glade_xml_new(PREFIX "/share/dssi_convolve/dssi_convolve_gtk.glade",  NULL, NULL);

	gtk_combo_box_set_active((GtkComboBox*)glade_xml_get_widget(xml, "partitionsize_combo"), 8);

	/* connect signal handlers */
	glade_xml_signal_autoconnect(xml);

	g_timeout_add(100, process_osc_messages, 0);

	sprintf(tmp_string, "%s/update", host_base_path);
	sprintf(tmp_string2, "%s", lo_server_get_url(server));

	printf("%s\n", tmp_string2);
	
	// lo_server_thread_start(server_thread);

	lo_send(host_address, tmp_string, "s", tmp_string2);

	gtk_window_set_title((GtkWindow*)glade_xml_get_widget(xml, "mainwindow"), argv[4]);


	gtk_main();

	return 0;
}
