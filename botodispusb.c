/**
 *	Botodispusb driver 
 *	
 *	Descripcio :
 *		-> carrega del modul 'init' : registrar-se com a driver usb
 *		-> descarrega del modul 'exit' : desregistrar-se com a driver usb, alliberar recursos
 *		-> deteccio de connexio de dispositiu 'botodispusb' : registrar el dispositiu i demanar recursos
 *									(endpoints, buffers, urbs, etc.)
 *		-> deteccio de desconnexio del dispositiu : alliberar recursos i desregistrar dispositiu
 *		-> controlar l'acces al dispositiu amb 'open' i 'release' : nomes pot entrar una aplicacio en tot moment
 *		-> enviament al dispositiu (display) dels bytes passats a traves la funcio d'escriptura
 *		-> recepcio dels bytes que genera el dispositiu (teclat) i reenviament cap al display
 *		-> esborrat del caracter anterior del display amb la tecla 'F'
 *
 *		Aquest driver esta suportat per la versio del kernel 2.6.XX.
 *
 *	DATA: 08/09/2010	
 */

/**
 *	Includes
 */
#include <linux/kernel.h>	/* printk, ...*/
#include <linux/errno.h>	/* ENODEV, ...*/
#include <linux/init.h>		/* __init, __exit */
#include <linux/slab.h>		/* kmalloc, kfree */
#include <linux/module.h>	/* necessari per qualsevol modul */
#include <linux/kref.h>		/* kref_init, kref_get, k_ref_put */
#include <linux/smp_lock.h>	/* lock_kernel, unlock_kernel */
#include <linux/usb.h>		/* totes les estructures i funcions relacionades amb l'USB */
#include <linux/uaccess.h>	/* get_user, copy_to_user, ...*/
#include <linux/semaphore.h>	/* init_MUTEX */
#include <linux/workqueue.h>	/* INIT_WORK, schedule_work, flush_work ... */
//#include <linux/wait.h>		/* wait_queu_head_t, wait_interruptible, wakeup */

/**
 *	DEFINES
 */
#define VENDOR_ID	0x04d8
#define PRODUCT_ID	0x00bd
#define NUM_FILES_DEF	20
//#define NUM_COLUMNES	40
#define kref_to_dev(r)	container_of(r, struct bdusb, bdu_refcount)
#define work_to_dev(w)	container_of(w, struct bdusb, t_teclat)

/** 
 *	Declaracio de funcions estructurals de qualsevol driver USB
 */
static int __init bdu_init(void);
static void __exit bdu_exit(void);
static int bdu_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void bdu_disconnect(struct usb_interface *interface);
static void bdu_delete(struct kref *bdu_kref);
static int bdu_open(struct inode *inode, struct file *file);
static int bdu_release(struct inode *inode, struct file *file);
static ssize_t bdu_write(struct file *file, const char *user_buffer, size_t count, loff_t *ppos);
static void bdu_out_callback(struct urb *bdu_urb, struct pt_regs *regs);
static ssize_t bdu_read(struct file *file, char *user_buffer, size_t count, loff_t *ppos);
static void bdu_in_callback(struct urb *bdu_urb, struct pt_regs *regs);

/**
 *	Declaracio de funcions especifiques d'aquest driver
 */
void Processar_tecla(struct work_struct *work);


/** 
 *	Estructura de dades general per a controlar el driver
 */
struct bdusb
{
	/* informacio basica de referencia usb */
	struct usb_device*	udev;
	struct usb_interface*	interface;
	struct kref		bdu_refcount;

	/* Les adreces dels endpoints */
	u8	bulk_out_endpointAddr;
	u8	interrupt_in_endpointAddr;

	/* Els buffers que es faran servir */	
	size_t		bulk_out_size;
	unsigned char*	bulk_out_buffer;
	size_t		interrupt_in_size;
	unsigned char*	interrupt_in_buffer;

	/* Els urbs que es faran servir */
	struct urb* 	urb_in_teclat;
	struct urb*	urb_out_display;

	/* variables de control del dispositiu */
	u8 num_access;			// numero d'accessos al dispositiu
	u8 col_actual;			// columna actual del cursor del display
	struct semaphore sem;		// semafor d'exclusio mutua
	struct work_struct t_teclat;	// treball per a controlar les pulsacions del teclat
	char codi_tecla;		// memoritza el codi de la tecla que ha de processar el treball anterior
};


/** 
 *	Informació estructural del driver usb
 */
static struct file_operations bdu_fops =
{
	.owner		=	THIS_MODULE,
	.read		=	bdu_read,
	.write		=	bdu_write,
	.open		=	bdu_open,
	.release	=	bdu_release
};

static struct usb_class_driver bdu_class =
{
	.name		=	"bd_usb",
	.fops		=	&bdu_fops,
	.minor_base	=	0
};

static struct usb_device_id taula_disp[] =
{
	{USB_DEVICE( VENDOR_ID, PRODUCT_ID)},
	{}
};

static struct usb_driver bdu_driver =
{
	.name		=	"botodispusb",
	.id_table 	=	taula_disp,
	.probe 		=	bdu_probe,
	.disconnect 	=	bdu_disconnect
};


/**
 *	DECLARACIO DE VARIABLES GLOBALS
 */
MODULE_DEVICE_TABLE(usb, taula_disp);
int num_files= NUM_FILES_DEF;



/**      
 *	bdu_init: s'invoca quan es carrega el driver al sistema (amb comanda 'insmod')
 */
static int __init bdu_init(void)
{
	int retval = 0;

	printk(KERN_INFO "BDUSB: __bdu_init__\n");

	retval = usb_register(&bdu_driver);
	if (retval) printk(KERN_ALERT " -> ERROR: usb_register ha fallat: numero d'error (%d)\n", retval);
	return retval;
}


/**	
 *	bdu_exit: s'invoca quan es descarrega el driver del sistema (amb comanda 'rmmod')
 */
static void __exit bdu_exit(void)
{
	printk(KERN_INFO "BDUSB: __bdu_exit__\n");

	usb_deregister(&bdu_driver);
}




/**
 *	bdu_probe: s'invoca quan es connecta un dispositiu USB del tipus que controla el driver (segons 'taula_disp')
 */
static int bdu_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct bdusb *dev= NULL;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int i, retval;

	printk(KERN_INFO "BDUSB: __bdu_probe__\n");

	/* demanem un espai de memoria per a l'estructura general de control del dispositiu i l'inicialitzem */
	dev = kmalloc(sizeof(struct bdusb), GFP_KERNEL);
	if (dev == NULL)
	{
		err(" -> ERROR: no hi ha memoria per a l'estructura 'dev'\n");
		return -ENOMEM;
	}
	/* inicialitza camps de l'estructura general 'dev' */
	dev->bulk_out_endpointAddr = 0;			// serveix per a que el bucle de deteccio i configuracio dels endpoints detecti
	dev->interrupt_in_endpointAddr = 0;		// que aquest endpoints encara no estan assignats (!dev->bulk_out_endpointAddr)

	/* inicialitza a u comptador de referencies a l'estructura */	
	kref_init(&dev->bdu_refcount);

	/* el camp 'udev' apuntara a l'estructura de dades que genera el sistema pel dispositiu connectat 'udev' */	
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	
	/* guardem la interficie del dispositiu */
	dev->interface = interface;

	/* configurem els endpoints (bulk-in and bulk-out endpoints) */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i)
	{
		endpoint = &(iface_desc->endpoint[i].desc);

		if (!dev->bulk_out_endpointAddr && !(endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK))
		{
			/* we found a bulk out endpoint */
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_out_size = endpoint->wMaxPacketSize;
			printk(KERN_INFO "BDUSB: endpoint bulk_out amb mida de buffer (%d)\n", dev->bulk_out_size);
			/* crear buffer per enviar dades al display */
			dev->bulk_out_buffer = kmalloc(dev->bulk_out_size, GFP_KERNEL);
			if (!dev->bulk_out_buffer)
			{
				err(" -> ERROR: no s'ha pogut crear bulk_out_buffer\n");
				kref_put(&dev->bdu_refcount, bdu_delete);
				return -ENOMEM;
			}
			/* crear urb per enviar dades al bulk_out_endpoint */
			dev->urb_out_display = usb_alloc_urb(0, GFP_KERNEL);
			if(!dev->urb_out_display)
			{
				err(" -> ERROR: no s'ha pogut crear el urb_out_display\n");
				kref_put(&dev->bdu_refcount, bdu_delete);
				return -ENOMEM;			
			}
		}
		if (!dev->interrupt_in_endpointAddr && (endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT))
		{
			/* we found an interrupt in endpoint */
			dev->interrupt_in_endpointAddr = endpoint->bEndpointAddress;
			dev->interrupt_in_size = endpoint->wMaxPacketSize;
			printk(KERN_INFO "BDUSB: endpoint interrupt_in amb mida de buffer (%d)\n", dev->interrupt_in_size);
			/* crear buffer per rebre dades del teclat */
			dev->interrupt_in_buffer = kmalloc(dev->interrupt_in_size, GFP_KERNEL);
			if (!dev->interrupt_in_buffer)
			{
				err(" -> ERROR: no s'ha pogut crear interrupt_in_buffer\n");
				kref_put(&dev->bdu_refcount, bdu_delete);
				return -ENOMEM;
			}
			/* crear urb per rebre dades pel interrupt_in_endpoint */
			dev->urb_in_teclat = usb_alloc_urb(0,GFP_KERNEL);
			if(!dev->urb_in_teclat)
			{
				err(" -> ERROR: no s'ha pogut crear el urb_in_teclat\n");
				kref_put(&dev->bdu_refcount, bdu_delete);
				return -ENOMEM;			
			}
		}
	}
	if (!(dev->interrupt_in_endpointAddr && dev->bulk_out_endpointAddr))
	{
		err("\n ERROR: no s'han aconseguit els dos endpoints necessaris (bulk_out i interrupt_in)\n");
		kref_put(&dev->bdu_refcount, bdu_delete);
		return -ENOMEM;
	}
	
	/* guardem un punter a l'estructura general 'dev' dins del camp de dades de la interficie de dispositiu */
	usb_set_intfdata(interface, dev);

	/* ara ja podem registrar el dispositiu, que estara associat a '/dev/btdispusb0' (si nomes controlarem un dispositiu) */
	retval = usb_register_dev(interface, &bdu_class);
	if (retval)
	{	/* something prevented us from registering this device */
		err("\n ERROR: no s'ha aconseguit un minor per al dispositiu \n");
		usb_set_intfdata(interface, NULL);
		kref_put(&dev->bdu_refcount, bdu_delete);
		return retval;
	}

	/* let the user know what node this device is now attached to */	
	printk(KERN_INFO "BDUSP: activat '/dev/btdispusb%d' amb Major (%d) Minor (%d)\n", interface->minor, USB_MAJOR, interface->minor);

	/* inicialitza variables de control */
	dev->num_access = 0;
	dev->col_actual = 0;
	/* inicialitzem el semafor general per a l'enviament d'urbs de sortida (bulk_out) */
	init_MUTEX(&dev->sem);
	/* inicialitza el treball per manegar la visualitzacio de les tecles premudes */
	INIT_WORK(&dev->t_teclat, Processar_tecla);

	/* inicialitza l'urb d'entrada per a captacio de la primera tecla (amb periode maxim de 250 mil·lisegons) */
	usb_fill_int_urb(dev->urb_in_teclat, dev->udev,
				usb_rcvintpipe(dev->udev, dev->interrupt_in_endpointAddr),
				dev->interrupt_in_buffer,
				dev->interrupt_in_size,
				(void*) bdu_in_callback, dev, 250);

	/* enviament de l'urb per captar la primera tecla pulsada */
	retval = usb_submit_urb(dev->urb_in_teclat, GFP_KERNEL);
	if (retval)
	{
		err("\n ERROR: no s'ha pogut enviar el primer URB de captacio de tecles\n");
		kref_put(&dev->bdu_refcount, bdu_delete);
		return retval;
	}
	return retval;
}


/**
 *	bdu_disconnect: s'invoca quan es desconnecta el dispositiu del bus USB
 */
static void bdu_disconnect(struct usb_interface *interface)
{
	struct bdusb *dev = (struct bdusb *) usb_get_intfdata(interface);

	printk(KERN_INFO "BDUSB: __bdu_disconnect__\n");
	
	/* espera que la cua de treballs estigui buida */
	flush_scheduled_work();
	
	/* desregistra el dispositiu (i el minor) */
	usb_deregister_dev(interface, &bdu_class);

	/* allibera l'us de l'estructura 'dev' i els recursos demanats */
	kref_put(&dev->bdu_refcount, bdu_delete);
}


/**
 * 	bdu_delete: s'invoca quan s'ha d'eliminar l'estructura del dispositiu i alliberar tots els recusos demanats
 */
static void bdu_delete(struct kref *bd_ref)
{
	struct bdusb *dev = kref_to_dev(bd_ref);

	printk(KERN_INFO "BDUSB: __bdu_delete__\n");

	/* allibera l'estructura d'informacio del dispositiu USB */
	usb_put_dev(dev->udev);

	/* allibera els recursos obtinguts (urbs i buffers) */
	if (dev->urb_in_teclat)		usb_free_urb(dev->urb_in_teclat);
	if (dev->urb_out_display)	usb_free_urb(dev->urb_out_display);
	if (dev->interrupt_in_buffer)	kfree(dev->interrupt_in_buffer);
	if (dev->bulk_out_buffer)	kfree(dev->bulk_out_buffer);
	kfree(dev);
}



/**
 *	bdu_open: s'invoca quan una aplicació intenta accedir al driver (p.ex., amb crida a 'fopen')
 */
static int bdu_open(struct inode *inode, struct file *file)
{
	struct bdusb *dev;
	struct usb_interface *interface;
	int subminor = iminor(inode);

	printk(KERN_INFO "BDUSB: __bdu_open__\n");

	interface = usb_find_interface(&bdu_driver, subminor);
	if (!interface)
	{
		err(" -> ERROR: no es pot detectar el dispositiu amb minor numero (%d)\n",subminor);
		return -ENODEV;
	}
	dev = usb_get_intfdata(interface);
	if (!dev)
	{
		err(" -> ERROR: no s'havia emmagatzemat res al camp de dades de la interficie de dispositiu\n");
		return -ENODEV;
	}
	if (dev->num_access > 0)
	{
		err(" -> ERROR: ja hi ha un acces al driver (%d)\n",subminor);
		return -ENODEV;
	}
	/* apuntar que hi ha un acces a l'estructura 'dev' */
	kref_get(&dev->bdu_refcount);
	/* memoritza l'adreça de l'estructura de dades del dispositiu per a les funcions 'read', 'write' i 'release' */
	file->private_data = dev;
	/* memoritzar que ja tenim un acces al dispositiu */
	dev->num_access = 1;
	return 0;
}


/**
 *	bdu_release: s'invoca quan una aplicacio allibera el seu acces al driver (p.ex., amb crida a 'fclose')
 */
static int bdu_release(struct inode *inode, struct file *file)
{
	struct bdusb *dev = (struct bdusb *) file->private_data;

	if (dev == NULL) return -ENODEV;

	printk(KERN_INFO "BDUSB: __bdu_release__\n");

	/* allibera l'acces a l'estructura 'dev' */
	kref_put(&dev->bdu_refcount, bdu_delete);
	/* memoritza que el dispositiu ja esta lliure per a tornar a ser accedit */
	dev->num_access = 0;
	return 0;
}



/**
 *	bdu_write: s'invoca quan una aplicacio envia informacio cap al driver (p.ex., amb crida a 'fwrite')
 *		Aquesta funcio envia cap al display fins a 16 bytes que se li passen pel buffer i retorna immediatament.
 */
static ssize_t bdu_write(struct file *file, const char *user_buffer, size_t count, loff_t *ppos)
{
	struct bdusb *dev = (struct bdusb *) file->private_data;	
	int i;
	char dada;
	
	printk(KERN_INFO "BDUSB: __bdu_write__\n");

	dev->bulk_out_buffer[0] = 0x01;		// preparem un paquet de dades
	i = 0;					// del punter d'escriptura a buffer
	while ((i < count) && (dev->col_actual < 16))		// per a tots els bytes (fins omplir la primera linia)
	{
		get_user(dada, &user_buffer[i]);	// captura un caracter
		dev->bulk_out_buffer[i+1] = dada;	// el memoritza al buffer d'enviament
		dev->col_actual++;			// apuntem el desplaçament automatic del cursor
		i++;
	}
	/* evitar utilitzar l'urb d'enviament si s'esta fent servir */	
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;		// retorna error si s'ha desbloquejat manualment amb Control-C

	/* preparem l'urb per a enviament de dades */
	usb_fill_bulk_urb(dev->urb_out_display, dev->udev,
			usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			dev->bulk_out_buffer, i+1, (void*) bdu_out_callback, dev);

	/* enviament de l'urb */
	if (usb_submit_urb(dev->urb_out_display, GFP_ATOMIC))
		printk(KERN_INFO " -> ERROR: no s'ha pogut enviar el paquet d'escriptura de dades");

	return count;	// retornem el numero de caracters de l'aplicacio, per a simular que els ha processat tots
}



/**
 *	bdu_out_callback: s'invoca quan el dispositiu ha acceptar un paquet de bytes enviat des de l'ordinador
 *			(una interrupcio per cada paquet)
 */
static void bdu_out_callback(struct urb *bdu_urb, struct pt_regs *regs)
{
	struct bdusb *dev = (struct bdusb *) bdu_urb->context;

	printk(KERN_INFO "BDUSB: __bdu_out_callback__ status (%d) \n", bdu_urb->status);
	switch(bdu_urb->status)
	{
		case 0:	/* s'ha enviat amb exit el paquet */
			up(&dev->sem);		// desbloqueja altres tasques que podrien estar esperant per enviar
			break;	
		case -ENOENT:
			/* file or directory(dev) cannot be found */
			err(" -> ERROR : NO es troba el dispositiu correcte\n");
			break;
		case -ECONNRESET:
			/* connection reset by peer */
			err(" -> ERROR : s'ha resetejat la connexio\n");
			break;
		case -ESHUTDOWN:
			/* cannot send after transport endpoint shutdown */
			printk(KERN_INFO " -> ERROR : s'ha desconnectat el dispositiu\n");
			usb_unlink_urb(bdu_urb);
			break;
	}
}




/**
 *	bdu_read: s'invoca quan una aplicacio demana informacio al driver (p.ex., amb crida a 'fread')
 *		Aquesta funcio actualment no fa res, pero hauria de retornar a l'aplicacio les tecles premudes.
 */
static ssize_t bdu_read(struct file *file, char *user_buffer, size_t count, loff_t *ppos)
{
	int retval= 0;
	//struct bdusb *dev= (struct bdusb *) file->private_data;

	printk(KERN_INFO "BDUSB: __bdu_read__\n");

	return retval;
}



/**
 *	bdu_in_callback: s'invoca quan el dispositiu envia un codi de tecla a l'ordinador a traves del cable USB
 *			(una interrupcio per tecla pitjada)
 */
static void bdu_in_callback(struct urb *bdu_urb, struct pt_regs *regs)
{
	struct bdusb *dev = (struct bdusb *) bdu_urb->context;

	printk(KERN_INFO "BDUSB: __bdu_in_callback__ codi (%c), status (%d) \n", dev->interrupt_in_buffer[0], bdu_urb->status);

	/*Comprovem si s'ha rebut el paquet correctament */
	switch (bdu_urb->status)
	{
		case 0: /* tractar la tecla rebuda */
			dev->codi_tecla = dev->interrupt_in_buffer[0];	// memoritza el codi ASCII de la tecla
			schedule_work(&dev->t_teclat);			// envia el work a la cua de treballs del sistema

			/* re-inicialitza l'urb de captacio de tecles */
			usb_fill_int_urb(bdu_urb, dev->udev,
					usb_rcvintpipe(dev->udev, dev->interrupt_in_endpointAddr),
					dev->interrupt_in_buffer,
					dev->interrupt_in_size,
					(void*) bdu_in_callback, dev, 250);
			/* i el torna a enviar */
			if (usb_submit_urb(bdu_urb, GFP_ATOMIC)) err(" -> ERROR: no s'ha pogut reenviar l'urb de lectura\n");
			break;	
		case -ENOENT:
			/* file or directory(dev) cannot be found */
			err(" -> ERROR : NO es troba el dispositiu correcte\n");
			break;
		case -ECONNRESET:
			/* connection reset by peer */
			err(" -> ERROR : s'ha resetejat la connexio\n");
			break;
		case -ESHUTDOWN:
			/* cannot send after transport endpoint shutdown */
			printk(KERN_INFO " -> ERROR : s'ha desconnectat el dispositiu\n");
			usb_unlink_urb(bdu_urb);
			break;
	}
}



void Processar_tecla(struct work_struct *work)
{
	char c;
	struct bdusb *dev = work_to_dev(work);		// obtenir l'adreça a l'estructura de dades del dispositiu

	c = dev->codi_tecla;
	if ((c == 'F') && (dev->col_actual > 0))	// tecla d'esborrat d'ultim caracter
	{
		if (down_interruptible(&dev->sem) == 0)
		{
			dev->bulk_out_buffer[0] = 0x00;	// preparem un paquet de comandes
			dev->col_actual--;		// decrementa la posicio del cursor en u
			dev->bulk_out_buffer[1] = 0x80 + dev->col_actual;	// comanda de posicionament del cursor
			usb_fill_bulk_urb(dev->urb_out_display, dev->udev,
					usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
					dev->bulk_out_buffer, 2, (void*) bdu_out_callback, dev);
			if (usb_submit_urb(dev->urb_out_display, GFP_ATOMIC)) printk(KERN_INFO "ERROR Processar tecla 1");
		}
		if (down_interruptible(&dev->sem) == 0)
		{
			dev->bulk_out_buffer[0] = 0x01;	// preparem un paquet de dades
			dev->bulk_out_buffer[1] = ' ';	// enviarem un espai en blanc per esborrar caracter anterior
			usb_fill_bulk_urb(dev->urb_out_display, dev->udev,
					usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
					dev->bulk_out_buffer, 2, (void*) bdu_out_callback, dev);
			if (usb_submit_urb(dev->urb_out_display, GFP_ATOMIC)) printk(KERN_INFO "ERROR Processar tecla 2");
			dev->col_actual++;		// apuntem el desplaçament automatic del cursor
		}
		if (down_interruptible(&dev->sem) == 0)
		{
			dev->bulk_out_buffer[0] = 0x00;	// preparem un paquet de comandes
			dev->col_actual--;		// recuperem la posicio del cursor despres de l'esborrat
			dev->bulk_out_buffer[1] = 0x80 + dev->col_actual;	// comanda de posicionament del cursor
			usb_fill_bulk_urb(dev->urb_out_display, dev->udev,
					usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
					dev->bulk_out_buffer, 2, (void*) bdu_out_callback, dev);
			if (usb_submit_urb(dev->urb_out_display, GFP_ATOMIC)) printk(KERN_INFO "ERROR Processar tecla 3");
		}
	}
	else if ((c != 'F') && (dev->col_actual < 16))
	{	// envia al display la tecla premuda
		if (down_interruptible(&dev->sem) == 0)
		{
			dev->bulk_out_buffer[0] = 0x01;	// preparem un paquet de dades
			dev->bulk_out_buffer[1] = c;
			usb_fill_bulk_urb(dev->urb_out_display, dev->udev,
					usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
					dev->bulk_out_buffer, 2, (void*) bdu_out_callback, dev);
			if (usb_submit_urb(dev->urb_out_display, GFP_ATOMIC)) printk(KERN_INFO "ERROR Processar tecla 4");
			dev->col_actual++;		// apuntem el desplaçament automatic del cursor
		}
	}
}



/**	
 *	Especificacio de les funcions d'inicialitzacio, finalitzacio i parametres del modul
 */
module_init(bdu_init);
module_exit(bdu_exit);
module_param(num_files, int, S_IRUGO);
/**	
 *	INFORMACIO sobre el modul
 */
MODULE_AUTHOR("Ferran Martinez i Santiago Romani");
MODULE_DESCRIPTION("BOTODISPUSB");
MODULE_LICENSE("GPL");

